// J1 の GPIO46・47・48（VDD_IO_5 のバンク）を出力 High にして、ピンの電圧をテスタで測る（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// 比べるために GPIO20（VDD_IO_0、3.3V のバンク）も High にする
// 段 1: LDO の VO4 には触らない（起動したままの状態）
// 段 2: VO4 を 3.3V で有効にする（ESP-IDF の SD カードの例と同じ LDO 4 番・3300mV）
// ピンには何もつながないこと
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"

#define STAGE1_SEC 30

static const int s_pins[] = {20, 46, 47, 48};  // J1-13, J1-36, J1-37, J1-33

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する

    uint64_t mask = 0;
    for (int i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        mask |= 1ULL << s_pins[i];
    }
    gpio_config_t out = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    for (int i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        gpio_set_level(s_pins[i], 1);
    }

    for (int t = STAGE1_SEC; t > 0; t -= 5) {
        printf("stage 1: GPIO20/46/47/48 = High, VO4 untouched  (%d s left)\n", t);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    esp_ldo_channel_handle_t vo4 = NULL;
    esp_ldo_channel_config_t cfg = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&cfg, &vo4));
    esp_ldo_dump(stdout);

    while (1) {
        printf("stage 2: GPIO20/46/47/48 = High, VO4 = 3300 mV\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
