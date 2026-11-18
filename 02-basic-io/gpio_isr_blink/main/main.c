// BOOT ボタン（GPIO35）の割り込みで、HP GPIO と LP GPIO の LED を反転する（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// LED（抵抗を直列に）: HP 側 GPIO20（J1 の 13 番）、LP 側 GPIO4（J1 の 18 番）。もう一方は GND へ
// BOOT ボタンはストラップのピン（GPIO35）。押したまま電源を入れたりリセットしたりしない
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#define PIN_BTN     35  // BOOT
#define PIN_LED_HP  20  // HP GPIO（GPIO16〜54）
#define PIN_LED_LP  4   // LP GPIO（GPIO0〜15）
#define DEBOUNCE_US 50000

static volatile int s_level;
static volatile uint32_t s_count;
static volatile int64_t s_last_us;

static void IRAM_ATTR on_button(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < DEBOUNCE_US) {
        return;  // チャタリング: 50 ms 以内の立ち下がりは無視
    }
    s_last_us = now;
    s_level = !s_level;
    gpio_set_level(PIN_LED_HP, s_level);
    gpio_set_level(PIN_LED_LP, s_level);
    s_count++;
}

static void blink_lp_with_rtc_api(void)
{
    // LP GPIO は LP IO MUX の API（rtc_gpio_*）でも動かせる（LP コアやスリープ中の保持はこちら）
    ESP_ERROR_CHECK(rtc_gpio_init(PIN_LED_LP));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(PIN_LED_LP, RTC_GPIO_MODE_OUTPUT_ONLY));
    for (int i = 0; i < 3; i++) {
        rtc_gpio_set_level(PIN_LED_LP, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        rtc_gpio_set_level(PIN_LED_LP, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_ERROR_CHECK(rtc_gpio_deinit(PIN_LED_LP));  // HP の GPIO マトリクス側に戻す
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する
    printf("GPIO%d: %s\n", PIN_LED_HP, rtc_gpio_is_valid_gpio(PIN_LED_HP) ? "LP GPIO" : "HP GPIO");
    printf("GPIO%d: %s\n", PIN_LED_LP, rtc_gpio_is_valid_gpio(PIN_LED_LP) ? "LP GPIO" : "HP GPIO");
    printf("GPIO%d: %s (BOOT)\n", PIN_BTN, rtc_gpio_is_valid_gpio(PIN_BTN) ? "LP GPIO" : "HP GPIO");

    printf("rtc_gpio API: blink GPIO%d x3\n", PIN_LED_LP);
    blink_lp_with_rtc_api();

    gpio_config_t led = {
        .pin_bit_mask = (1ULL << PIN_LED_HP) | (1ULL << PIN_LED_LP),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(PIN_LED_HP, 0);
    gpio_set_level(PIN_LED_LP, 0);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // ボードにも 10kΩ のプルアップがある
        .intr_type = GPIO_INTR_NEGEDGE,    // 押した瞬間（立ち下がり）
    };
    ESP_ERROR_CHECK(gpio_config(&btn));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_BTN, on_button, NULL));
    printf("gpio API: press BOOT to toggle GPIO%d and GPIO%d\n", PIN_LED_HP, PIN_LED_LP);

    uint32_t shown = 0;
    while (1) {
        uint32_t c = s_count;
        if (c != shown) {
            shown = c;
            printf("press %" PRIu32 ": LED %s\n", c, s_level ? "ON" : "OFF");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
