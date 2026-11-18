// SPI で LIS3DH を読む（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// SPI2 の IO MUX のピン（GPIO6〜11）は J1 にほとんど出ていないので、GPIO マトリクス経由で J1 のピンを使う
// 配線（I2C の線は外す）: VCC → 3V3、GND → GND、
//   SCL(SPC) → GPIO32（J1 の 26 番）、SDA/SDI(MOSI) → GPIO33（29 番）、SDO(MISO) → GPIO23（7 番）、CS → GPIO2（21 番）
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/spi_master.h"

#define HOST     SPI2_HOST
#define PIN_SCLK 32
#define PIN_MOSI 33
#define PIN_MISO 23
#define PIN_CS   2
#define CLOCK_HZ (5 * 1000 * 1000)  // LIS3DH は 10 MHz まで

#define REG_WHO_AM_I 0x0F
#define REG_CTRL1    0x20
#define REG_CTRL4    0x23
#define REG_OUT_X_L  0x28
#define SPI_READ     0x80  // 1 バイト目の bit7: 読み
#define SPI_INC      0x40  // 1 バイト目の bit6: アドレスを進めて連続読み

static spi_device_handle_t s_dev;

// 1 バイト目にコマンド、続く n バイトでデータを読む（全二重）
static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    uint8_t tx[8] = {0}, rx[8] = {0};
    tx[0] = SPI_READ | (n > 1 ? SPI_INC : 0) | reg;
    spi_transaction_t t = {.length = (n + 1) * 8, .tx_buffer = tx, .rx_buffer = rx};
    esp_err_t err = spi_device_transmit(s_dev, &t);
    memcpy(buf, rx + 1, n);
    return err;
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA, .length = 16, .tx_data = {reg, val}};
    return spi_device_transmit(s_dev, &t);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する

    spi_bus_config_t bc = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HOST, &bc, SPI_DMA_DISABLED));

    spi_device_interface_config_t dc = {
        .clock_speed_hz = CLOCK_HZ,
        .mode = 3,  // CPOL=1、CPHA=1
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(HOST, &dc, &s_dev));
    int khz = 0;
    spi_device_get_actual_freq(s_dev, &khz);
    printf("SPI clock: %d kHz\n", khz);

    uint8_t who = 0;
    ESP_ERROR_CHECK(rd(REG_WHO_AM_I, &who, 1));
    printf("WHO_AM_I = 0x%02X (expect 0x33)\n", who);

    ESP_ERROR_CHECK(wr(REG_CTRL1, 0x57));  // 100 Hz、X・Y・Z 有効
    ESP_ERROR_CHECK(wr(REG_CTRL4, 0x88));  // BDU=1、高分解能（12 ビット）、±2 g
    vTaskDelay(pdMS_TO_TICKS(20));

    while (1) {
        uint8_t b[6];
        ESP_ERROR_CHECK(rd(REG_OUT_X_L, b, sizeof(b)));
        int x = (int16_t)(b[0] | (b[1] << 8)) >> 4;
        int y = (int16_t)(b[2] | (b[3] << 8)) >> 4;
        int z = (int16_t)(b[4] | (b[5] << 8)) >> 4;
        printf("x=%5d y=%5d z=%5d mg\n", x, y, z);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
