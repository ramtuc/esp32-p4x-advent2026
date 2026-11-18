// 新しい I2C ドライバ（driver/i2c_master.h）で、ボードの I2C バスをスキャンして LIS3DH を読む
// （ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// 配線: VCC → 3V3、GND → GND、SDA → GPIO7（J1 の 3 番）、SCL → GPIO8（J1 の 5 番）。CS・SDO は何もつながない
// このバスにはボードのコーデック（ES8311）なども載っている
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#define PIN_SDA     7
#define PIN_SCL     8
#define LIS3DH_ADDR 0x19  // SDO/SA0 が VCC（モジュールの既定）。GND なら 0x18

#define REG_WHO_AM_I 0x0F  // 0x33 が返る
#define REG_CTRL1    0x20
#define REG_CTRL4    0x23
#define REG_OUT_X_L  0x28
#define AUTO_INC     0x80  // I2C では副アドレスの最上位ビットで連続読み

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static void scan(void)
{
    printf("scan:");
    int found = 0;
    for (uint16_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_bus, a, 50) == ESP_OK) {  // 応答なしは ESP_ERR_NOT_FOUND
            printf(" 0x%02X", a);
            found++;
        }
    }
    printf("  (%d devices)\n", found);
}

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, sizeof(b), 100);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する

    i2c_master_bus_config_t bc = {
        .i2c_port = -1,  // 空いている I2C を自動で選ぶ
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));
    scan();

    if (i2c_master_probe(s_bus, LIS3DH_ADDR, 50) != ESP_OK) {
        printf("LIS3DH (0x%02X) not found\n", LIS3DH_ADDR);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            scan();
        }
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LIS3DH_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dc, &s_dev));

    uint8_t who = 0;
    ESP_ERROR_CHECK(rd(REG_WHO_AM_I, &who, 1));
    printf("WHO_AM_I = 0x%02X (expect 0x33)\n", who);

    ESP_ERROR_CHECK(wr(REG_CTRL1, 0x57));  // 100 Hz、X・Y・Z 有効
    ESP_ERROR_CHECK(wr(REG_CTRL4, 0x88));  // BDU=1、高分解能（12 ビット）、±2 g
    vTaskDelay(pdMS_TO_TICKS(20));

    while (1) {
        uint8_t b[6];
        esp_err_t err = rd(REG_OUT_X_L | AUTO_INC, b, sizeof(b));
        if (err != ESP_OK) {
            printf("read: %s\n", esp_err_to_name(err));  // v6.0 では NACK が ESP_ERR_INVALID_RESPONSE
        } else {
            // 左詰めの 12 ビット。±2 g の高分解能モードで 1 mg/digit
            int x = (int16_t)(b[0] | (b[1] << 8)) >> 4;
            int y = (int16_t)(b[2] | (b[3] << 8)) >> 4;
            int z = (int16_t)(b[4] | (b[5] << 8)) >> 4;
            printf("x=%5d y=%5d z=%5d mg\n", x, y, z);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
