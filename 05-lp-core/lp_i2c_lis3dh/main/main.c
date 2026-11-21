// HP コアを Deep Sleep にして、LP コアに LP I2C で LIS3DH を見張らせる。傾いたら LP コアが HP を起こす
// （ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3。下敷きは examples/system/ulp/lp_core/lp_i2c）
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "lp_core_i2c.h"
#include "ulp_lp_core.h"
#include "ulp_main.h"  // LP 側の変数（ulp_ax_mg など）。ビルドのときに作られる

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

// EV-Board の J1: GPIO4 = J1-18、GPIO5 = J1-16（どちらも LP IO の GPIO0〜15 に入る）
#define LP_I2C_SDA GPIO_NUM_4
#define LP_I2C_SCL GPIO_NUM_5
#define LP_PERIOD_US 100000   // LP タイマで LP コアを起こす間隔（100ms）
#define THRESHOLD_MG 500      // X か Y が 500 mg を超えたら「傾いた」
#define WATCH_SEC 10          // 電源を入れたあと、HP も起きたまま LP の変数を表示する時間

static RTC_DATA_ATTR uint32_t s_boots;

static const char *reason_str(uint32_t r)
{
    switch (r) {
    case 1: return "sensor ready";
    case 2: return "tilt";
    case 3: return "I2C error";
    default: return "-";
    }
}

static void print_scan(void)
{
    printf("    scan (SDA=GPIO%d SCL=GPIO%d):", LP_I2C_SDA, LP_I2C_SCL);
    int n = 0;
    for (int a = 0x08; a < 0x78; a++) {
        if (ulp_scan_found[a / 32] & (1u << (a % 32))) {
            printf(" 0x%02X", a);
            n++;
        }
    }
    printf("%s\n", n ? "" : " (none)");
}

static void print_lp_vars(void)
{
    printf("    run_count=%" PRIu32 " x=%5" PRId32 " y=%5" PRId32 " z=%5" PRId32 " mg"
           " WHO_AM_I=0x%02" PRIX32 " addr=0x%02" PRIX32 " errors=%" PRIu32 " (last %" PRId32 ")\n",
           (uint32_t)ulp_run_count, (int32_t)ulp_ax_mg, (int32_t)ulp_ay_mg, (int32_t)ulp_az_mg,
           (uint32_t)ulp_who_am_i, (uint32_t)ulp_i2c_addr, (uint32_t)ulp_err_count, (int32_t)ulp_last_err);
}

static void start_lp_core(void)
{
    // LP コアが使う LP I2C は、HP が先に設定しておく
    lp_core_i2c_cfg_t i2c_cfg = LP_CORE_I2C_DEFAULT_CONFIG();
    i2c_cfg.i2c_pin_cfg.sda_io_num = LP_I2C_SDA;
    i2c_cfg.i2c_pin_cfg.scl_io_num = LP_I2C_SCL;
    ESP_ERROR_CHECK(lp_core_i2c_master_init(LP_I2C_NUM_0, &i2c_cfg));

    ESP_ERROR_CHECK(ulp_lp_core_load_binary(ulp_main_bin_start, ulp_main_bin_end - ulp_main_bin_start));
    ulp_threshold_mg = THRESHOLD_MG;
    ulp_lp_core_cfg_t cfg = {
        .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
        .lp_timer_sleep_duration_us = LP_PERIOD_US,
    };
    ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));  // USB の再接続を待ってから表示する
    s_boots++;

    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_ULP)) {
        printf("[%" PRIu32 "] woken by LP core: %s\n", s_boots, reason_str(ulp_notify));
        print_lp_vars();
        if (ulp_notify != 2) {
            print_scan();
        }
    } else {
        printf("[%" PRIu32 "] not a LP core wakeup: start LP core (every %d ms), threshold %d mg\n",
               s_boots, LP_PERIOD_US / 1000, THRESHOLD_MG);
        start_lp_core();
        // LP コアは HP が起きている間も 100ms ごとに動く。しばらく LP の変数を見せてから眠る
        for (int i = 0; i < WATCH_SEC; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            print_lp_vars();
        }
        print_scan();
    }

    ulp_notify = 0;  // 読んだので、LP コアの「起こす」を止める
    printf("    HP enters deep sleep\n");
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
    esp_deep_sleep_start();
}
