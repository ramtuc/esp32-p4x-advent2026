/*
 * Deep Sleep からの復帰（タイマと GPIO）と、起きた理由のログ
 * ESPr Developer S3 Type-C（ESP32-S3）/ ESP-IDF v6.0.3
 *
 * - タイマ: 10 秒ごとに起きる
 * - GPIO : FLASH スイッチ（GPIO0）を押して Low になったら起きる（EXT1）
 * - RTC メモリに起きた回数を残し、Deep Sleep に入った時刻との差で眠っていた時間を出す
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <inttypes.h>
#include <sys/time.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WAKE_GPIO       GPIO_NUM_0          /* FLASH スイッチ（押すと Low） */
#define TIMER_WAKE_SEC  10

/* RTC SLOW メモリに置く変数は、Deep Sleep の間も消えない（電源を切る・リセットでは消える） */
static RTC_DATA_ATTR uint32_t s_boot_count;
static RTC_DATA_ATTR struct timeval s_sleep_enter;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "OTHER";
    }
}

static void print_wakeup_causes(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();   /* v6.0 からはビットの集まりで返る */
    printf("wakeup causes = 0x%08" PRIx32 " :", causes);
    if (causes & BIT(ESP_SLEEP_WAKEUP_UNDEFINED)) printf(" UNDEFINED(not from deep sleep)");
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER))     printf(" TIMER");
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        uint64_t pins = esp_sleep_get_ext1_wakeup_status();
        printf(" EXT1(pin mask=0x%llx)", pins);
    }
    if (causes & BIT(ESP_SLEEP_WAKEUP_ULP))       printf(" ULP");
    printf("\n");
}

void app_main(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    s_boot_count++;
    printf("\n=== boot #%" PRIu32 ", reset reason = %s ===\n",
           s_boot_count, reset_reason_str(esp_reset_reason()));
    print_wakeup_causes();

    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        int64_t slept_ms = (now.tv_sec - s_sleep_enter.tv_sec) * 1000LL
                         + (now.tv_usec - s_sleep_enter.tv_usec) / 1000LL;
        printf("slept %lld ms\n", slept_ms);
    }

    /* 押されたままなら離されるまで待つ（押しっぱなしで起き続けないように） */
    rtc_gpio_init(WAKE_GPIO);
    rtc_gpio_set_direction(WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(WAKE_GPIO);
    rtc_gpio_pulldown_dis(WAKE_GPIO);
    while (rtc_gpio_get_level(WAKE_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* 起こす元を 2 つ登録する */
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)TIMER_WAKE_SEC * 1000000ULL));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(BIT64(WAKE_GPIO), ESP_EXT1_WAKEUP_ANY_LOW));
    /* RTC の周辺（プルアップ）を Deep Sleep の間も生かしておく */
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));

    printf("enter deep sleep (timer %d s or GPIO%d low)\n", TIMER_WAKE_SEC, WAKE_GPIO);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));   /* UART の送信が終わるのを待つ */

    gettimeofday(&s_sleep_enter, NULL);
    esp_deep_sleep_start();
}
