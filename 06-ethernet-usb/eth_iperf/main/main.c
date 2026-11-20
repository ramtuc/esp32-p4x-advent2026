/*
 * ESP32-P4 の EMAC と IP101（PHY）で Ethernet を起こし、iperf のコンソールを開く（ESP-IDF v6.0.3）。
 * コンソールは J19（USB Serial/JTAG）。例:
 *   iperf -s -i 1                       ボードがサーバー（PC → ボード）
 *   iperf -c <PC の IP> -i 1 -t 20      ボードがクライアント（ボード → PC）
 *   iperf -u -c <PC の IP> -b 100M -t 20
 * iperf は iperf 2 と互換（iperf3 とは話せない）。PC 側は iperf 2 を使う。
 * IP アドレスは表示する（PC から ボードにつなぐのに使う）。記事に載せるときは伏せる。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_eth.h"
#include "esp_eth_phy_ip101.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_console.h"
#include "iperf_cmd.h"

// ESP-IDF の Ethernet の例（examples/ethernet/basic）の ESP32-P4 向けの既定と同じ
#define PHY_ADDR     1
#define PHY_RST_GPIO 51
#define MDC_GPIO     31
#define MDIO_GPIO    52

#define GOT_IP BIT0
static EventGroupHandle_t s_ev;

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_eth_handle_t eth = *(esp_eth_handle_t *)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed;
        eth_duplex_t duplex;
        esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        printf("link up: %s, %s duplex\n", speed == ETH_SPEED_100M ? "100Mbps" : "10Mbps",
               duplex == ETH_DUPLEX_FULL ? "full" : "half");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        printf("link down\n");
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    printf("got IP " IPSTR "\n", IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_ev, GOT_IP);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));  // J19 の USB を PC が開き直すのを待ってから表示する
    s_ev = xEventGroupCreate();

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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));

    xEventGroupWaitBits(s_ev, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";
    esp_console_dev_usb_serial_jtag_config_t usj = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usj, &repl_config, &repl));
    esp_console_register_help_command();
    iperf_cmd_register_iperf();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
