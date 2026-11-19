/*
 * TWAI（CAN）の 1 マスタ＋複数スレーブ。ESP-IDF v6.0.3 の新しいドライバ esp_twai を使う
 * ESPr Developer S3 Type-C（ESP32-S3）＋ CAN トランシーバ
 *
 * 役割は menuconfig の「CAN node」で選ぶ:
 *   - マスタ: 100ms ごとに問い合わせ（ID 0x100）を出し、スレーブの返事（0x200〜0x20F）を受ける
 *   - スレーブ: 問い合わせだけを受け（受け入れフィルタ）、すぐ 0x200 + ノード番号で返事をする
 *     → 全スレーブが同時に送り出すので、ID の小さい方が勝つ（バス調停）
 *   - セルフテスト: トランシーバなしで 1 枚で試す。TX と RX を同じピンにし、自分の送ったフレームを受ける
 *
 * バスオフ（送信エラーが重なって、ノードがバスから外れた状態）になったら twai_node_recover() で戻す。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

#define POLL_ID        0x100
#define REPLY_BASE_ID  0x200
#define POLL_PERIOD_MS 100

typedef struct {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[TWAI_FRAME_MAX_LEN];
    int64_t  t_us;     /* 受けた時刻（esp_timer） */
} rx_msg_t;

static QueueHandle_t s_rxq;
static volatile bool s_bus_off;

static const char *state_name(twai_error_state_t s)
{
    static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
    return (s <= TWAI_ERROR_BUS_OFF) ? names[s] : "?";
}

/* 受けたフレームを割り込みの中で取り出し、キューに入れる */
static bool IRAM_ATTR on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *ctx)
{
    rx_msg_t m = {0};
    twai_frame_t f = {.buffer = m.data, .buffer_len = sizeof(m.data)};
    BaseType_t woken = pdFALSE;
    if (twai_node_receive_from_isr(node, &f) == ESP_OK) {
        m.id = f.header.id;
        m.len = twaifd_dlc2len(f.header.dlc);
        m.t_us = esp_timer_get_time();
        xQueueSendFromISR(s_rxq, &m, &woken);
    }
    return woken == pdTRUE;
}

/* エラーの状態が変わったとき（error_active → error_warning → error_passive → bus_off） */
static bool IRAM_ATTR on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t *edata, void *ctx)
{
    ESP_EARLY_LOGW("can", "state: %s -> %s", state_name(edata->old_sta), state_name(edata->new_sta));
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_bus_off = true;
    }
    return false;
}

static bool IRAM_ATTR on_error(twai_node_handle_t node, const twai_error_event_data_t *edata, void *ctx)
{
    ESP_EARLY_LOGW("can", "bus error flags=0x%" PRIx32, edata->err_flags.val);
    return false;
}

static twai_node_handle_t start_node(bool self_test)
{
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = CONFIG_CAN_TX_GPIO,
            /* セルフテストは TX と RX を同じピンにして、出した信号をそのまま受ける */
            .rx = self_test ? CONFIG_CAN_TX_GPIO : CONFIG_CAN_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CONFIG_CAN_BITRATE,
        .fail_retry_cnt = -1,          /* 調停に負けたら、送れるまで送り直す（CAN のふつうの動き） */
        .tx_queue_depth = 8,
        .flags.enable_self_test = self_test,   /* ほかのノードの ACK（受け取りの合図）がなくても送れたことにする */
        .flags.enable_loopback = self_test,    /* 自分が送ったフレームを自分でも受ける */
    };
    twai_node_handle_t node;
    ESP_ERROR_CHECK(twai_new_node_onchip(&cfg, &node));
    return node;
}

static void enable_node(twai_node_handle_t node)
{
    twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
        .on_error = on_error,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node, &cbs, NULL));
    ESP_ERROR_CHECK(twai_node_enable(node));
}

/* 受け入れフィルタ（ID と mask。mask の 1 のビットだけ比べる）。ノードを有効にする前に設定する */
static void set_filter(twai_node_handle_t node, uint32_t id, uint32_t mask)
{
    twai_mask_filter_config_t f = {
        .id = id,
        .mask = mask,
        .is_ext = false,   /* 標準フォーマット（11 ビットの ID） */
    };
    ESP_ERROR_CHECK(twai_node_config_mask_filter(node, 0, &f));
    printf("filter: id=0x%03" PRIx32 " mask=0x%03" PRIx32 "\n", id, mask);
}

static void send(twai_node_handle_t node, uint32_t id, const uint8_t *data, size_t len)
{
    twai_frame_t f = {
        .header.id = id,
        .header.dlc = len,
        .buffer = (uint8_t *)data,
        .buffer_len = len,
    };
    esp_err_t err = twai_node_transmit(node, &f, 100);
    if (err != ESP_OK) {
        printf("transmit 0x%03" PRIx32 " failed: %s\n", id, esp_err_to_name(err));
        return;
    }
    /* 送信は非同期。f と data（どちらもこの関数のローカル）は送り終わるまで生きていないといけないので、
       ここで送り終わりを待つ（ESP-IDF の TWAI の説明「frame and buffer must remain valid until the transmission is actually complete」） */
    err = twai_node_transmit_wait_all_done(node, 100);
    if (err != ESP_OK) {
        printf("transmit 0x%03" PRIx32 " not done: %s\n", id, esp_err_to_name(err));
    }
}

static void check_bus_off(twai_node_handle_t node)
{
    if (s_bus_off) {
        s_bus_off = false;
        printf("bus off -> twai_node_recover()\n");
        /* すぐには戻らない。ESP-IDF の説明では、11 ビット続く劣性（recessive）を 129 回見てから、
           コントローラが自分でバスに戻る */
        ESP_ERROR_CHECK(twai_node_recover(node));
    }
}

static void print_info(twai_node_handle_t node)
{
    twai_node_status_t st;
    twai_node_record_t rec;
    ESP_ERROR_CHECK(twai_node_get_info(node, &st, &rec));
    printf("state=%s TEC=%u REC=%u bus_err=%" PRIu32 "\n",
           state_name(st.state), st.tx_error_count, st.rx_error_count, rec.bus_err_num);
}

#if CONFIG_CAN_NODE_ROLE_MASTER
static void run(twai_node_handle_t node)
{
    uint16_t seq = 0;
    uint32_t got[16] = {0}, polls = 0;
    for (;;) {
        uint8_t d[2] = {seq & 0xff, seq >> 8};
        int64_t t0 = esp_timer_get_time();
        send(node, POLL_ID, d, sizeof(d));
        polls++;

        /* 返事を受けた順に並べる（調停に勝った順） */
        char order[64] = "";
        rx_msg_t m;
        while (xQueueReceive(s_rxq, &m, pdMS_TO_TICKS(POLL_PERIOD_MS / 2)) == pdTRUE) {
            uint16_t rseq = m.data[0] | (m.data[1] << 8);
            if (rseq != seq) continue;
            int n = m.id - REPLY_BASE_ID;
            got[n & 15]++;
            size_t l = strlen(order);
            snprintf(order + l, sizeof(order) - l, " %d(+%lldus)", n, m.t_us - t0);
        }
        if (seq % 10 == 0) {
            printf("poll %u: replies in order:%s\n", seq, order[0] ? order : " none");
        }
        if (seq % 50 == 0) {
            printf("received / polls:");
            for (int i = 1; i < 16; i++) if (got[i]) printf(" node%d=%" PRIu32 "/%" PRIu32, i, got[i], polls);
            printf("\n");
            print_info(node);
        }
        check_bus_off(node);
        seq++;
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS / 2));
    }
}
#elif CONFIG_CAN_NODE_ROLE_SLAVE
static void run(twai_node_handle_t node)
{
    uint32_t polls = 0;
    for (;;) {
        rx_msg_t m;
        if (xQueueReceive(s_rxq, &m, pdMS_TO_TICKS(1000)) == pdTRUE && m.id == POLL_ID) {
            /* 返事: 問い合わせの番号、ノード番号、受けた問い合わせの数 */
            uint8_t d[8] = {m.data[0], m.data[1], CONFIG_CAN_NODE_ID,
                            polls & 0xff, (polls >> 8) & 0xff, (polls >> 16) & 0xff, polls >> 24, 0};
            send(node, REPLY_BASE_ID + CONFIG_CAN_NODE_ID, d, sizeof(d));
            if (++polls % 50 == 0) {
                printf("node %d: %" PRIu32 " polls answered\n", CONFIG_CAN_NODE_ID, polls);
                print_info(node);
            }
        }
        check_bus_off(node);
    }
}
#else /* セルフテスト */
static void run(twai_node_handle_t node)
{
    /* マスタと同じフィルタ（0x200〜0x20F だけ受ける）で、いろいろな ID を送って受けたものを見る */
    const uint32_t ids[] = {0x100, 0x201, 0x202, 0x20F, 0x210, 0x305};
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            uint8_t d[2] = {round, i};
            send(node, ids[i], d, sizeof(d));
        }
        ESP_ERROR_CHECK(twai_node_transmit_wait_all_done(node, 1000));
        vTaskDelay(pdMS_TO_TICKS(50));
        printf("round %d: sent", round);
        for (int i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) printf(" 0x%03" PRIx32, ids[i]);
        printf("\n         received");
        rx_msg_t m;
        while (xQueueReceive(s_rxq, &m, 0) == pdTRUE) printf(" 0x%03" PRIx32, m.id);
        printf("\n");
    }
    print_info(node);
    check_bus_off(node);
    printf("self test done\n");
    for (;;) vTaskDelay(portMAX_DELAY);
}
#endif

void app_main(void)
{
    s_rxq = xQueueCreate(32, sizeof(rx_msg_t));
#if CONFIG_CAN_NODE_ROLE_SELFTEST
    const bool self_test = true;
    printf("\nrole=self test, pin=GPIO%d, %d bit/s\n", CONFIG_CAN_TX_GPIO, CONFIG_CAN_BITRATE);
#else
    const bool self_test = false;
#if CONFIG_CAN_NODE_ROLE_MASTER
    const char *role = "master";
#else
    const char *role = "slave";
#endif
    printf("\nrole=%s, TX=GPIO%d RX=GPIO%d, %d bit/s\n", role,
           CONFIG_CAN_TX_GPIO, CONFIG_CAN_RX_GPIO, CONFIG_CAN_BITRATE);
#endif
    twai_node_handle_t node = start_node(self_test);

#if CONFIG_CAN_NODE_ROLE_SLAVE
    set_filter(node, POLL_ID, 0x7FF);              /* 0x100 だけ */
#else
    set_filter(node, REPLY_BASE_ID, 0x7F0);        /* 0x200〜0x20F */
#endif
    enable_node(node);
    run(node);
}
