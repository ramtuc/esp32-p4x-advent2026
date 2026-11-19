// HP コアを Deep Sleep にして、LP コアだけを動かす。LP コアが HP を起こしたら、共有の変数を読んで表示する
// （ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "ulp_lp_core.h"
#include "ulp_main.h"  // LP 側の変数（ulp_run_count など）。ビルドのときに作られる

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

#define LP_PERIOD_US 100000  // LP タイマで LP コアを起こす間隔（100ms）

// RTC_DATA_ATTR の変数は Deep Sleep の間も消えない（LP SRAM に置かれる）
static RTC_DATA_ATTR uint64_t s_last_us;
static RTC_DATA_ATTR uint32_t s_last_runs;
static RTC_DATA_ATTR uint32_t s_boots;

static void start_lp_core(void)
{
    ESP_ERROR_CHECK(ulp_lp_core_load_binary(ulp_main_bin_start, ulp_main_bin_end - ulp_main_bin_start));
    ulp_wake_every = 50;  // HP から LP の変数に書く: 50 回（約 5 秒）ごとに起こして
    ulp_lp_core_cfg_t cfg = {
        .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
        .lp_timer_sleep_duration_us = LP_PERIOD_US,
    };
    ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));  // USB の再接続を待ってから表示する
    uint64_t now = esp_rtc_get_time_us();
    s_boots++;

    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_ULP)) {
        uint32_t runs = ulp_run_count;
        printf("[%" PRIu32 "] woken by LP core: run_count=%" PRIu32 " wake_count=%" PRIu32 " wake_every=%" PRIu32 "\n",
               s_boots, runs, (uint32_t)ulp_wake_count, (uint32_t)ulp_wake_every);
        if (s_last_us != 0 && runs > s_last_runs) {
            uint64_t dt = now - s_last_us;
            printf("    since last wake: %" PRIu64 " ms, %" PRIu32 " LP runs, %.1f ms per LP run\n",
                   dt / 1000, runs - s_last_runs, (double)dt / 1000.0 / (runs - s_last_runs));
        }
        if (ulp_wake_count == 3) {
            ulp_wake_every = 20;  // HP から LP の変数を書き換える: 次からは 20 回（約 2 秒）ごと
            printf("    HP sets wake_every = 20\n");
        }
    } else {
        printf("[%" PRIu32 "] not a LP core wakeup: start LP core (every %d ms)\n", s_boots, LP_PERIOD_US / 1000);
        start_lp_core();
    }

    s_last_us = esp_rtc_get_time_us();
    s_last_runs = ulp_run_count;
    printf("    HP enters deep sleep\n");
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
    esp_deep_sleep_start();
}
