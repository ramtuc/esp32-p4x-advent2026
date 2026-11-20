// ESP32-P4X-Function-EV-Board のオンボードの ESP32-C6 を、ESP-Hosted（SDIO）経由で Wi-Fi のステーションとして使う（ESP-IDF v6.0.3）
// P4 側は esp_wifi_remote で、ふつうの esp_wifi の API をそのまま呼ぶ。中身は SDIO で C6 に送られる
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
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "esp_hosted_host_fw_ver.h"

#define GOT_IP BIT0

static EventGroupHandle_t s_ev;

#ifdef WIFI_SSID
static int64_t s_connect_us;  // 直前の esp_wifi_connect() の時刻

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK (SAE)";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    default: return "other";
    }
}

static void connect_now(void)
{
    s_connect_us = esp_timer_get_time();
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
#ifndef CONFIG_WIFI_STA_CONNECT_AFTER_SETUP
        connect_now();
#endif
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        // SSID・BSSID は出さない。理由・その時の RSSI・connect からの時間だけ
        printf("disconnected (reason %d, rssi %d dBm, %lld ms after connect), retry\n", d->reason, d->rssi,
               (esp_timer_get_time() - s_connect_us) / 1000);
        vTaskDelay(pdMS_TO_TICKS(2000));
        connect_now();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *c = data;
        printf("associated: authmode %s, %lld ms after connect\n", auth_name(c->authmode),
               (esp_timer_get_time() - s_connect_us) / 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, GOT_IP);
    }
}
#endif

static void print_versions(void)
{
    printf("ESP-Hosted host version: %d.%d.%d\n", ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
           ESP_HOSTED_VERSION_PATCH_1);
    esp_hosted_coprocessor_fwver_t v = {0};
    esp_err_t err = esp_hosted_get_coprocessor_fwversion(&v) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        printf("ESP-Hosted co-processor (C6) version: %lu.%lu.%lu\n", (unsigned long)v.major1,
               (unsigned long)v.minor1, (unsigned long)v.patch1);
    } else {
        printf("ESP-Hosted co-processor (C6) version: not available\n");
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ev = xEventGroupCreate();

#ifndef WIFI_SSID
    // SSID が無いビルドでは、電波を出す前の確認（版の表示）だけで止まる
    print_versions();
    printf("WIFI_SSID is not set at build time: stop here (no connection)\n");
    return;
#else
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    print_versions();

    // チャンネルは 1〜11 だけ使う（特例の条件。12・13ch は外国の認証の範囲の外）
    wifi_country_t country = {.cc = "JP", .schan = 1, .nchan = 11, .policy = WIFI_COUNTRY_POLICY_MANUAL};
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

#ifdef CONFIG_WIFI_STA_DIAG_LOG
    // 調査用: P4 側の ESP-Hosted のログを DEBUG に（Wi-Fi のドライバのログは C6 の UART に出るので見えない）
    const char *tags[] = {"RPC_WRAP", "rpc_core", "rpc_evt", "rpc_rsp", "rpc_req", "rpc_api", "H_API"};
    for (int i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_DEBUG);
    }
#endif

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    // WPA3（SAE）の設定。既定は {0} のまま（Hunt-and-Peck、しきい値 OPEN）。Kconfig で変えて比べる
    wc.sta.sae_pwe_h2e = CONFIG_WIFI_STA_SAE_PWE;
#if CONFIG_WIFI_STA_AUTH_THRESHOLD_WPA2
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#elif CONFIG_WIFI_STA_AUTH_THRESHOLD_WPA3
    wc.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
#endif
    printf("sta config: sae_pwe_h2e %d, threshold %s, pmf capable %d required %d\n", wc.sta.sae_pwe_h2e,
           auth_name(wc.sta.threshold.authmode), wc.sta.pmf_cfg.capable, wc.sta.pmf_cfg.required);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    int8_t pwr = 0;
    // ホストが上限をかける前の、C6 のファームの上限（slave の CONFIG_ESP_PHY_MAX_WIFI_TX_POWER）
    esp_wifi_get_max_tx_power(&pwr);
    printf("max tx power (C6 firmware, before set): %.2f dBm\n", pwr * 0.25);
    // 送信出力の上限を +15dBm に（単位は 0.25dBm。60 = 15dBm）
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(60));
    esp_wifi_get_max_tx_power(&pwr);
    printf("max tx power: %.2f dBm\n", pwr * 0.25);
#ifdef CONFIG_WIFI_STA_CONNECT_AFTER_SETUP
    // 送信出力の設定の RPC が終わってから接続を始める
    connect_now();
#endif

    xEventGroupWaitBits(s_ev, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    wifi_ap_record_t ap;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap));
    // SSID・BSSID・IP アドレスは出さない（ログを記事に載せるため）。つながったことと無線の条件だけ
    printf("connected: channel %d, rssi %d dBm, phy 11b%d 11g%d 11n%d 11ax%d\n", ap.primary, ap.rssi,
           ap.phy_11b, ap.phy_11g, ap.phy_11n, ap.phy_11ax);
    printf("got IPv4 address: %s\n", ip.ip.addr != 0 ? "yes" : "no");

    // phy_11ax はアクセスポイントの対応。実際にどの規格で話しているかは、ネゴシエーションの結果を読む
    wifi_phy_mode_t mode;
    esp_err_t err = esp_wifi_sta_get_negotiated_phymode(&mode);
    if (err == ESP_OK) {
        printf("negotiated phy mode: %s\n", mode == WIFI_PHY_MODE_HE20   ? "11ax (HE20)"
                                            : mode == WIFI_PHY_MODE_HT40 ? "11n (HT40)"
                                            : mode == WIFI_PHY_MODE_HT20 ? "11n (HT20)"
                                            : mode == WIFI_PHY_MODE_11G  ? "11g"
                                            : mode == WIFI_PHY_MODE_11B  ? "11b"
                                                                         : "other");
    } else {
        printf("negotiated phy mode: not available (%s)\n", esp_err_to_name(err));
    }
#endif
}
