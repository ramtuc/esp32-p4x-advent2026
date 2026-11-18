// UART1 を J1 のピンに出して、ジャンパ線でループバックする（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// TX: GPIO21（J1 の 11 番）、RX: GPIO22（J1 の 12 番）。11 番と 12 番をジャンパ線でつなぐ
// UART0（GPIO37/38、J1 の 8・10 番）はコンソールなので使わない
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/uart.h"

#define PORT   UART_NUM_1
#define PIN_TX 21
#define PIN_RX 22

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PORT, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
    // v6.0 の uart_set_pin は 5 引数（TX・RX・RTS・CTS）と 7 引数（DTR・DSR も）の両方を受ける
    ESP_ERROR_CHECK(uart_set_pin(PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    for (int n = 1;; n++) {
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "hello %d", n);
        uart_flush_input(PORT);
        uart_write_bytes(PORT, msg, len);
        char rx[32] = {0};
        int got = uart_read_bytes(PORT, rx, len, pdMS_TO_TICKS(100));
        if (got < 0) {
            got = 0;
        }
        printf("sent \"%s\"  received %d bytes \"%.*s\"  %s\n", msg, got, got, rx,
               (got == len && memcmp(msg, rx, len) == 0) ? "OK" : "NG");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
