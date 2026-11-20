// ESP32-P4X-Function-EV-Board のオンボードの ESP32-C6（ESP-Hosted、SDIO）で Wi-Fi につなぎ、iperf と ping のコンソールを開く（ESP-IDF v6.0.3）
// コンソールは J19（USB Serial/JTAG）。例: `iperf -s -i 1`、`iperf -c <PC の IP> -i 1 -t 30`、`iperf -u -c <PC の IP> -b 60M -t 30`、`ping <IP>`
// iperf は iperf 2 と互換（iperf3 とは話せない）。PC 側は iperf 2 を使う
//
// 技適未取得機器を用いた実験等の特例の条件に合わせて、チャンネルは 1〜11、送信出力は +15dBm 以下にしている
// SSID とパスワードは、ビルドのときの環境変数 WIFI_SSID・WIFI_PASS から渡す（ログにも出さない）
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "iperf_cmd.h"
#include "ping_cmd.h"

#ifndef WIFI_SSID
#error "WIFI_SSID・WIFI_PASS をビルドのときの環境変数で渡してください"
#endif

#define GOT_IP BIT0

static EventGroupHandle_t s_ev;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        printf("disconnected (reason %d), retry\n", d->reason);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, GOT_IP);
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ev = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // チャンネルは 1〜11 だけ使う（特例の条件）
    wifi_country_t country = {.cc = "JP", .schan = 1, .nchan = 11, .policy = WIFI_COUNTRY_POLICY_MANUAL};
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 送信出力の上限を +15dBm に（単位は 0.25dBm。60 = 15dBm）
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(60));
    // スループットを測るので、省電力（モデムスリープ）を切る
    esp_wifi_set_ps(WIFI_PS_NONE);

    xEventGroupWaitBits(s_ev, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    wifi_ap_record_t ap;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap));
    // SSID・BSSID・IP アドレスは出さない（ログを記事に載せるため）
    printf("connected: channel %d, rssi %d dBm\n", ap.primary, ap.rssi);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";
    esp_console_dev_usb_serial_jtag_config_t usj = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usj, &repl_config, &repl));
    esp_console_register_help_command();
    iperf_cmd_register_iperf();
    ping_cmd_register_ping();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
