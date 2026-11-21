/*
 * ESP32-P4 の EMAC と IP101（PHY）で Ethernet を起こし、次を表示する。
 * - MDIO で読んだ PHY の ID（ケーブルが無くても読める）
 * - リンクの状態（BMSR）、リンクが上がったら速さと全二重／半二重
 * - DHCP で取れた IP アドレス
 * MAC アドレスは表示しない。
 *
 * CONFIG_ETH_LINK_WATCH=y（sdkconfig.watch）にすると止まらずに動き続け、
 * ケーブルの抜き差しで起きる出来事（リンクの上下・IP の取得と喪失）と、
 * BMSR が変わった瞬間を表示する。
 *
 * ESP-IDF v6.0 から、IP101 などの PHY ごとのドライバは IDF 本体から外れ、
 * コンポーネントレジストリの espressif/ip101 になった。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_eth.h"
#include "esp_eth_phy_ip101.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "eth_link";
static esp_eth_handle_t s_eth;

// ESP-IDF の Ethernet の例（examples/ethernet/basic）の ESP32-P4 向けの既定と同じ
#define PHY_ADDR     1
#define PHY_RST_GPIO 51
#define MDC_GPIO     31
#define MDIO_GPIO    52

static uint32_t read_phy(esp_eth_handle_t eth, uint32_t reg)
{
    uint32_t val = 0;
    esp_eth_phy_reg_rw_data_t rw = {.reg_addr = reg, .reg_value_p = &val};
    if (esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &rw) != ESP_OK) {
        return 0xFFFFFFFF;
    }
    return val;
}

static void log_bmsr(const char *why)
{
    uint32_t bmsr = read_phy(s_eth, 1);
    ESP_LOGI(TAG, "%sBMSR=0x%04" PRIx32 " link=%d autoneg_done=%d", why, bmsr,
             (int)((bmsr >> 2) & 1), (int)((bmsr >> 5) & 1));
}

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_eth_handle_t eth = *(esp_eth_handle_t *)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        eth_speed_t speed;
        eth_duplex_t duplex;
        esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGI(TAG, "link up: %s, %s duplex", speed == ETH_SPEED_100M ? "100Mbps" : "10Mbps",
                 duplex == ETH_DUPLEX_FULL ? "full" : "half");
#if CONFIG_ETH_LINK_WATCH
        log_bmsr("  at link up: ");
#endif
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
#if CONFIG_ETH_LINK_WATCH
        log_bmsr("  at link down: ");
#endif
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "started");
        break;
    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGI(TAG, "lost IP");
        return;
    }
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got IP " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
}

void app_main(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();  // RMII のピンは既定のまま
    emac_config.smi_gpio.mdc_num = MDC_GPIO;
    emac_config.smi_gpio.mdio_num = MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = PHY_ADDR;
    phy_config.reset_gpio_num = PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));
    s_eth = eth;

    // PHY の ID（レジスタ 2・3）。IEEE 802.3 の決まりで、上位に OUI（メーカーの番号）が入る
    uint32_t id1 = read_phy(eth, 2), id2 = read_phy(eth, 3);
    ESP_LOGI(TAG, "PHY addr %d: ID1=0x%04" PRIx32 " ID2=0x%04" PRIx32 " (model %" PRIu32 ", rev %" PRIu32 ")",
             PHY_ADDR, id1, id2, (id2 >> 4) & 0x3F, id2 & 0xF);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
#if CONFIG_ETH_LINK_WATCH
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip, NULL));
#endif
    ESP_ERROR_CHECK(esp_eth_start(eth));

#if CONFIG_ETH_LINK_WATCH
    // 止まらずに BMSR を見張り、値が変わったときだけ表示する（30 秒ごとに生存の印も出す）
    log_bmsr("watch start: ");
    uint32_t last = read_phy(eth, 1);
    TickType_t last_alive = xTaskGetTickCount();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ETH_LINK_POLL_MS));
        uint32_t bmsr = read_phy(eth, 1);
        if (bmsr != last) {
            ESP_LOGI(TAG, "BMSR changed 0x%04" PRIx32 " -> 0x%04" PRIx32 " link=%d autoneg_done=%d", last, bmsr,
                     (int)((bmsr >> 2) & 1), (int)((bmsr >> 5) & 1));
            last = bmsr;
        }
        if (xTaskGetTickCount() - last_alive >= pdMS_TO_TICKS(30000)) {
            last_alive = xTaskGetTickCount();
            log_bmsr("alive: ");
        }
    }
#endif
    // 5 秒ごとに BMSR（レジスタ 1）を読む。bit2 = リンク、bit5 = オートネゴシエーション完了
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t bmsr = read_phy(eth, 1);
        ESP_LOGI(TAG, "BMSR=0x%04" PRIx32 " link=%d autoneg_done=%d", bmsr, (int)((bmsr >> 2) & 1), (int)((bmsr >> 5) & 1));
    }
    ESP_LOGI(TAG, "done");
}
