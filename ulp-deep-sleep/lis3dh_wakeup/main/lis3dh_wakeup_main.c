/*
 * 加速度センサ LIS3DH の INT1 で Deep Sleep から起こす
 * ESPr Developer S3 Type-C（ESP32-S3）/ ESP-IDF v6.0.3
 *
 * - LIS3DH（I2C、0x19）に「動いたら INT1 を High にする」設定を入れる
 * - 起こす元は 3 つ
 *     EXT1  : INT1（GPIO12）が High
 *     EXT0  : FLASH スイッチ（GPIO0）が Low
 *     タイマ: 60 秒
 * - 起きたら理由・EXT1 の pin mask・LIS3DH の INT1_SRC を出して、また眠る
 * - LIS3DH が見つからないときは、EXT1 を使わずに GPIO0 とタイマだけで眠る
 *
 * 配線（ESPr のシルク）: 3V3-VCC、GND-GND、10-SDA/SDI、11-SCL、12-I1（INT1）
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_SDA_GPIO    GPIO_NUM_10
#define I2C_SCL_GPIO    GPIO_NUM_11
#define INT1_GPIO       GPIO_NUM_12         /* LIS3DH の INT1（動いたら High） */
#define BUTTON_GPIO     GPIO_NUM_0          /* FLASH スイッチ（押すと Low） */
#define TIMER_WAKE_SEC  60

#define LIS3DH_ADDR     0x19                /* SDO/SA0 が High（モジュールの 10kΩ）のとき */

/* LIS3DH のレジスタ（ST のデータシート Doc ID 17530 Rev 1 の 8 章） */
#define REG_WHO_AM_I        0x0F            /* 0x33 が返る */
#define REG_CTRL_REG1       0x20            /* 出力のデータレートと、使う軸 */
#define REG_CTRL_REG2       0x21            /* ハイパスフィルタ */
#define REG_CTRL_REG3       0x22            /* INT1 に何を出すか */
#define REG_CTRL_REG4       0x23            /* フルスケール */
#define REG_CTRL_REG5       0x24            /* INT1 のラッチ */
#define REG_CTRL_REG6       0x25            /* INT の極性 */
#define REG_INT1_CFG        0x30            /* 割り込み 1 の条件 */
#define REG_INT1_SRC        0x31            /* 割り込み 1 の原因（読むと INT1 が Low に戻る） */
#define REG_INT1_THS        0x32            /* 割り込み 1 のしきい値 */
#define REG_INT1_DURATION   0x33            /* 割り込み 1 の最小の長さ */

/* RTC SLOW メモリに置く変数は、Deep Sleep の間も消えない */
static RTC_DATA_ATTR uint32_t s_boot_count;
static RTC_DATA_ATTR uint32_t s_wake_ext1, s_wake_ext0, s_wake_timer;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t lis3dh_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t lis3dh_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static bool lis3dh_open(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  /* モジュールにも 4.7kΩ のプルアップがある */
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    if (i2c_master_probe(s_bus, LIS3DH_ADDR, 100) != ESP_OK) {
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LIS3DH_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    uint8_t who = 0;
    if (lis3dh_read(REG_WHO_AM_I, &who) != ESP_OK || who != 0x33) {
        printf("LIS3DH: WHO_AM_I = 0x%02x (expected 0x33)\n", who);
        return false;
    }
    return true;
}

/* 動いたら INT1 を High にする設定（電源投入・リセットのときだけ入れる。
   Deep Sleep の間も LIS3DH は 3.3V で動き続け、設定は残る） */
static void lis3dh_setup_motion_int1(void)
{
    ESP_ERROR_CHECK(lis3dh_write(REG_CTRL_REG1, 0x57)); /* 100Hz、ノーマルモード、X・Y・Z を使う */
    ESP_ERROR_CHECK(lis3dh_write(REG_CTRL_REG2, 0x81)); /* ハイパスフィルタを割り込み 1 の判定に通す
                                                           （重力のような動かない成分を引く） */
    ESP_ERROR_CHECK(lis3dh_write(REG_CTRL_REG4, 0x00)); /* ±2g */
    ESP_ERROR_CHECK(lis3dh_write(REG_CTRL_REG6, 0x00)); /* INT はアクティブ High */
    ESP_ERROR_CHECK(lis3dh_write(REG_INT1_THS, 0x10));  /* しきい値 */
    ESP_ERROR_CHECK(lis3dh_write(REG_INT1_DURATION, 0x00)); /* 1 回でも超えたら */
    ESP_ERROR_CHECK(lis3dh_write(REG_CTRL_REG5, 0x08)); /* INT1 をラッチ（INT1_SRC を読むまで High のまま） */
    ESP_ERROR_CHECK(lis3dh_write(REG_INT1_CFG, 0x2A));  /* X・Y・Z のどれかが「しきい値より上」（OR） */
    ESP_ERROR_CHECK(lis3dh_write(REG_CTRL_REG3, 0x40)); /* 割り込み 1（AOI1）を INT1 のピンに出す */
}

static uint8_t lis3dh_clear_int1(void)
{
    uint8_t src = 0;
    ESP_ERROR_CHECK(lis3dh_read(REG_INT1_SRC, &src));   /* 読むとラッチが外れ、INT1 が Low に戻る */
    return src;
}

static void print_int1_src(const char *label, uint8_t src)
{
    printf("%s INT1_SRC = 0x%02x :%s%s%s%s%s%s%s\n", label, src,
           (src & 0x40) ? " IA" : "",
           (src & 0x20) ? " ZH" : "", (src & 0x10) ? " ZL" : "",
           (src & 0x08) ? " YH" : "", (src & 0x04) ? " YL" : "",
           (src & 0x02) ? " XH" : "", (src & 0x01) ? " XL" : "");
}

static void print_wakeup_causes(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    printf("wakeup causes = 0x%08" PRIx32 " :", causes);
    if (causes & BIT(ESP_SLEEP_WAKEUP_UNDEFINED)) printf(" UNDEFINED(not from deep sleep)");
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) { printf(" TIMER"); s_wake_timer++; }
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT0))  { printf(" EXT0(GPIO%d)", BUTTON_GPIO); s_wake_ext0++; }
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        printf(" EXT1(pin mask=0x%llx)", esp_sleep_get_ext1_wakeup_status());
        s_wake_ext1++;
    }
    printf("\n");
}

void app_main(void)
{
    s_boot_count++;
    printf("\n=== boot #%" PRIu32 " ===\n", s_boot_count);
    print_wakeup_causes();

    bool have_lis3dh = lis3dh_open();
    if (!have_lis3dh) {
        printf("LIS3DH not found at 0x%02x: wake by GPIO%d or timer only\n", LIS3DH_ADDR, BUTTON_GPIO);
    } else if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        lis3dh_setup_motion_int1();
        printf("LIS3DH found at 0x%02x (WHO_AM_I = 0x33), INT1 = motion (latched)\n", LIS3DH_ADDR);
    }

    if (have_lis3dh) {
        /* 起こした原因を読む。読むと INT1 が Low に戻る */
        print_int1_src("LIS3DH", lis3dh_clear_int1());
    }
    printf("wake count: EXT1(INT1) = %" PRIu32 ", EXT0(GPIO0) = %" PRIu32 ", TIMER = %" PRIu32 "\n",
           s_wake_ext1, s_wake_ext0, s_wake_timer);

    /* ボタンが押されたままなら離されるまで待つ（押しっぱなしで起き続けないように） */
    rtc_gpio_init(BUTTON_GPIO);
    rtc_gpio_set_direction(BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    while (rtc_gpio_get_level(BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (have_lis3dh) {
        /* 揺れが収まるのを少し待ってから、もう一度ラッチを外す */
        vTaskDelay(pdMS_TO_TICKS(500));
        lis3dh_clear_int1();

        /* INT1 は LIS3DH が High/Low を出す（押し引きの出力）。外れているときに浮かないよう、プルダウンも付ける */
        rtc_gpio_init(INT1_GPIO);
        rtc_gpio_set_direction(INT1_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(INT1_GPIO);
        rtc_gpio_pulldown_en(INT1_GPIO);
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(BIT64(INT1_GPIO), ESP_EXT1_WAKEUP_ANY_HIGH));
    }
    /* GPIO0 はボードの 10kΩ で 3.3V に引かれている。押すと Low */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)TIMER_WAKE_SEC * 1000000ULL));

    printf("enter deep sleep (%stimer %d s, GPIO%d low)\n",
           have_lis3dh ? "INT1 high, " : "", TIMER_WAKE_SEC, BUTTON_GPIO);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));   /* UART の送信が終わるのを待つ */
    esp_deep_sleep_start();
}
