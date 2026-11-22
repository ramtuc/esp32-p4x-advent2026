/*
 * ULP RISC-V にボタンを見張らせて、メインの CPU は Deep Sleep で寝かせておく
 * ESPr Developer S3 Type-C（ESP32-S3）/ ESP-IDF v6.0.3
 *
 * - ULP: 20ms ごとに GPIO0（FLASH スイッチ）を見て、3 回押されたらメインを起こす
 * - メイン: 起きたら理由と ULP の数え値を出して、また寝る
 *           念のため 60 秒のタイマでも起きる（ULP が動き続けていることを確かめる）
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <inttypes.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/rtc_io.h"
#include "ulp_riscv.h"
#include "ulp_main.h"          /* ULP 側の大域変数（ulp_run_count など）。ビルド時に作られる */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_GPIO       GPIO_NUM_0
#define ULP_PERIOD_US     20000          /* ULP を起こす間隔 20ms */
#define TIMER_WAKE_SEC    60

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[]   asm("_binary_ulp_main_bin_end");

/* 眠りに入った時刻と、そのときの ULP の run_count（Deep Sleep の間も消えない） */
static RTC_DATA_ATTR struct timeval s_sleep_enter;
static RTC_DATA_ATTR uint32_t s_run_at_sleep;

static void start_ulp(void)
{
    /* GPIO0 を RTC GPIO にして、ULP から読めるようにする（内蔵プルアップ） */
    rtc_gpio_init(BUTTON_GPIO);
    rtc_gpio_set_direction(BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(BUTTON_GPIO);
    rtc_gpio_pullup_en(BUTTON_GPIO);

    ESP_ERROR_CHECK(ulp_riscv_load_binary(ulp_main_bin_start,
                                          ulp_main_bin_end - ulp_main_bin_start));
    ulp_set_wakeup_period(0, ULP_PERIOD_US);
    ESP_ERROR_CHECK(ulp_riscv_run());
}

void app_main(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    uint32_t causes = esp_sleep_get_wakeup_causes();

    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        /* 電源投入・リセット: ULP のプログラムを RTC SLOW メモリに入れて動かす */
        printf("\nfirst boot: load ULP program (%u bytes), period %d ms\n",
               (unsigned)(ulp_main_bin_end - ulp_main_bin_start), ULP_PERIOD_US / 1000);
        start_ulp();
    } else {
        printf("\nwoke up by:%s%s\n",
               (causes & BIT(ESP_SLEEP_WAKEUP_ULP))   ? " ULP"   : "",
               (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) ? " TIMER" : "");
    }

    /* ULP の数え値（ULP が書いた RTC SLOW メモリをそのまま読む） */
    printf("ULP run_count = %" PRIu32 ", press_count = %" PRIu32 " (wake at %" PRIu32 "), low_samples = %" PRIu32 "\n",
           ulp_run_count, ulp_press_count, ulp_wake_presses, ulp_low_samples);

    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        /* 眠っていた時間と、その間に ULP が起こされた回数から、ULP の間隔を出す */
        int64_t slept_us = (now.tv_sec - s_sleep_enter.tv_sec) * 1000000LL
                         + (now.tv_usec - s_sleep_enter.tv_usec);
        uint32_t runs = ulp_run_count - s_run_at_sleep;
        printf("slept %lld ms, ULP ran %" PRIu32 " times -> %lld us per run\n",
               slept_us / 1000, runs, runs ? slept_us / runs : 0);
    }

    if (causes & BIT(ESP_SLEEP_WAKEUP_ULP)) {
        ulp_press_count = 0;   /* 次の 3 回を数え直す */
    }

    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
    /* RTC GPIO（入力とプルアップ）は RTC_PERIPH という電源の区画にある。
       起こす元が ULP とタイマだけだと、ESP-IDF はこの区画を寝ている間に切ってしまい、
       ULP が GPIO0 を読めない（ずっと High に見える）。明示して入れたままにする */
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)TIMER_WAKE_SEC * 1000000ULL));

    printf("enter deep sleep (ULP keeps running)\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));

    gettimeofday(&s_sleep_enter, NULL);
    s_run_at_sleep = ulp_run_count;
    esp_deep_sleep_start();
}
