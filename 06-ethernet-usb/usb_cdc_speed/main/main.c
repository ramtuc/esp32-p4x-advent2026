/*
 * USB CDC-ACM（仮想シリアル）の転送速度を測るためのデバイス側。
 * ESP-IDF v6.0.3 の examples/peripherals/usb/device/tusb_serial_device をもとにした。
 *
 * PC 側（tools/usb_cdc_speed.py）から 1 行のコマンドを送る:
 *   "I\n"       -> "INFO port=HS|FS speed=high|full|low ep=<..> rx=<..> tx=<..>\n"
 *   "W <n>\n"   -> 続く n バイトを受けて捨て、"OK W <n> <us>\n" を返す（us: 最初から最後のバイトまでの時間）
 *   "R <n>\n"   -> n バイトを送る（中身は 32 ビットの通し番号 0, 1, 2, ... のリトルエンディアン）。そのあとに何も付けない
 *
 * CONFIG_USB_SPEED_USE_FS=y にすると、High-Speed ではなく Full-Speed の USB OTG を使う。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "sdkconfig.h"

static const char *TAG = "usb_speed";

#define ITF 0

#if CONFIG_USB_SPEED_USE_FS
#define PORT_NAME "FS"
#else
#define PORT_NAME "HS"
#endif

typedef enum { ST_CMD, ST_SINK } state_t;

static state_t s_state = ST_CMD;
static char s_line[48];
static size_t s_line_len;
static uint32_t s_sink_total, s_sink_got;
static int64_t s_sink_t0;
static uint8_t s_rx[CONFIG_TINYUSB_CDC_RX_BUFSIZE];

static QueueHandle_t s_send_q;       // "R <n>" の n を送るタスクへ渡す
static SemaphoreHandle_t s_tx_done;  // 1 回の送りが終わった合図（FIFO に空きができた）
static uint32_t s_pattern[1024];   // 送るデータ。4096 バイトずつ通し番号を書き込む

static const char *speed_name(void)
{
    switch (tud_speed_get()) {
    case TUSB_SPEED_HIGH: return "high";
    case TUSB_SPEED_FULL: return "full";
    case TUSB_SPEED_LOW:  return "low";
    default:              return "unknown";
    }
}

static void reply(const char *s)
{
    tinyusb_cdcacm_write_queue(ITF, (const uint8_t *)s, strlen(s));
    tinyusb_cdcacm_write_flush(ITF, 0);
}

static void handle_line(void)
{
    char out[192];
    s_line[s_line_len] = '\0';
    s_line_len = 0;
    if (s_line[0] == 'I') {
        snprintf(out, sizeof(out), "INFO port=%s speed=%s ep=%d rx=%d tx=%d\n", PORT_NAME, speed_name(),
                 CONFIG_TINYUSB_CDC_EP_BUFSIZE, CONFIG_TINYUSB_CDC_RX_BUFSIZE, CONFIG_TINYUSB_CDC_TX_BUFSIZE);
        reply(out);
    } else if (s_line[0] == 'W') {
        s_sink_total = strtoul(s_line + 1, NULL, 10);
        s_sink_got = 0;
        if (s_sink_total) {
            s_state = ST_SINK;
        }
    } else if (s_line[0] == 'R') {
        uint32_t n = strtoul(s_line + 1, NULL, 10);
        xQueueSend(s_send_q, &n, 0);
    }
}

// TinyUSB のタスクから呼ばれる。測る間は、受けたものを数えて捨てるだけ
static void on_rx(int itf, cdcacm_event_t *event)
{
    size_t got = 0;
    if (tinyusb_cdcacm_read(itf, s_rx, sizeof(s_rx), &got) != ESP_OK) {
        return;
    }
    size_t i = 0;
    while (i < got) {
        if (s_state == ST_SINK) {
            if (s_sink_got == 0) {
                s_sink_t0 = esp_timer_get_time();
            }
            size_t take = got - i;
            if (take > s_sink_total - s_sink_got) {
                take = s_sink_total - s_sink_got;
            }
            s_sink_got += take;
            i += take;
            if (s_sink_got >= s_sink_total) {
                int64_t us = esp_timer_get_time() - s_sink_t0;
                char out[64];
                snprintf(out, sizeof(out), "OK W %" PRIu32 " %" PRId64 "\n", s_sink_total, us);
                reply(out);
                ESP_LOGI(TAG, "W %" PRIu32 " bytes in %" PRId64 " us", s_sink_total, us);
                s_state = ST_CMD;
            }
        } else {
            char c = (char)s_rx[i++];
            if (c == '\n') {
                handle_line();
            } else if (c != '\r' && s_line_len < sizeof(s_line) - 1) {
                s_line[s_line_len++] = c;
            }
        }
    }
}

// DTR が立っていないと、TinyUSB は送りの FIFO を「上書きしてよい」にする（cdc_device.c）ので表示して確かめる
static void on_line_state(int itf, cdcacm_event_t *event)
{
    ESP_LOGI(TAG, "line state: DTR=%d RTS=%d", event->line_state_changed_data.dtr, event->line_state_changed_data.rts);
}

// TinyUSB の弱い関数を上書きする。CDC の送りが 1 回終わるたびに呼ばれる
void tud_cdc_tx_complete_cb(uint8_t itf)
{
    xSemaphoreGive(s_tx_done);
}

static void sender_task(void *arg)
{
    uint32_t n;
    while (1) {
        xQueueReceive(s_send_q, &n, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        uint32_t sent = 0, word = 0;
        while (sent < n) {
            for (int i = 0; i < 1024; i++) {
                s_pattern[i] = word + i;
            }
            word += 1024;
            uint32_t len = n - sent < sizeof(s_pattern) ? n - sent : sizeof(s_pattern);
            const uint8_t *p = (const uint8_t *)s_pattern;
            uint32_t off = 0;
            while (off < len) {
                uint32_t q = tud_cdc_n_write(ITF, p + off, len - off);
                tud_cdc_n_write_flush(ITF);
                if (q == 0) {
                    xSemaphoreTake(s_tx_done, 1);  // FIFO がいっぱい。送りが終わるのを待つ
                    continue;
                }
                off += q;
            }
            sent += len;
        }
        while (tud_cdc_n_write_available(ITF) < CONFIG_TINYUSB_CDC_TX_BUFSIZE) {
            tud_cdc_n_write_flush(ITF);
            xSemaphoreTake(s_tx_done, 1);
        }
        ESP_LOGI(TAG, "R %" PRIu32 " bytes in %" PRId64 " us", n, esp_timer_get_time() - t0);
    }
}

static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "attached: port=%s speed=%s", PORT_NAME, speed_name());
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(TAG, "detached");
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));  // J19 を PC が開き直すまで待つ（それまでの表示は捨てられる）
    s_send_q = xQueueCreate(2, sizeof(uint32_t));
    s_tx_done = xSemaphoreCreateBinary();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(on_usb_event);  // ESP32-P4 の既定は High-Speed の口
#if CONFIG_USB_SPEED_USE_FS
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &on_rx,
        .callback_line_state_changed = &on_line_state,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    ESP_LOGI(TAG, "ready: port=%s ep=%d rx=%d tx=%d", PORT_NAME,
             CONFIG_TINYUSB_CDC_EP_BUFSIZE, CONFIG_TINYUSB_CDC_RX_BUFSIZE, CONFIG_TINYUSB_CDC_TX_BUFSIZE);

    xTaskCreate(sender_task, "sender", 4096, NULL, 4, NULL);
}
