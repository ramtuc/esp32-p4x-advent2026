// LP コアのプログラム: LP I2C で LIS3DH を読み、傾いたら HP コアを起こす
// LP タイマで 100ms ごとに main() が 1 回動き、戻ると止まる
// （ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3。下敷きは examples/system/ulp/lp_core/lp_i2c）
#include <stdint.h>
#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_i2c.h"

#define REG_WHO_AM_I 0x0F  // 0x33 が返る
#define REG_CTRL1    0x20
#define REG_CTRL4    0x23
#define REG_OUT_X_L  0x28
#define AUTO_INC     0x80  // レジスタの番号の最上位を 1 にすると、続けて読める
#define I2C_TIMEOUT_CYCLES 5000

// 通知の理由（HP が notify を読んで表示する）
#define NOTIFY_READY 1  // センサの設定が終わり、最初の値が読めた
#define NOTIFY_TILT  2  // しきい値を超えて傾いた
#define NOTIFY_ERROR 3  // I2C の最初のエラー

volatile uint32_t i2c_addr;           // 最初の回のスキャンで見つけた LIS3DH のアドレス（0x18 か 0x19）
volatile uint32_t scan_found[4];      // スキャンで応答したアドレス（0x00〜0x7F のビット）
volatile uint32_t scan_done;
volatile int32_t threshold_mg = 500;  // HP が書く。X か Y がこれを超えたら「傾いた」
volatile uint32_t run_count;
volatile uint32_t wake_count;
volatile uint32_t err_count;
volatile int32_t last_err;
volatile uint32_t who_am_i;
volatile uint32_t sensor_ready;
volatile int32_t ax_mg, ay_mg, az_mg;
volatile uint32_t tilted;
volatile uint32_t notify;  // 0 以外の間は HP を起こし続ける。HP が読んだら 0 に戻す

// 「傾いた」「戻った」の記録（最新の 16 件。HP が起きたときにまとめて表示する）
#define EV_MAX 16
volatile uint32_t ev_n;            // これまでの件数
volatile uint32_t ev_run[EV_MAX];  // そのときの run_count
volatile int32_t ev_x[EV_MAX], ev_y[EV_MAX], ev_z[EV_MAX];
volatile uint32_t ev_tilt[EV_MAX]; // 1 = 傾いた、0 = 水平に戻った

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return lp_core_i2c_master_write_to_device(LP_I2C_NUM_0, i2c_addr, buf, sizeof(buf), I2C_TIMEOUT_CYCLES);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    uint8_t r = (n > 1) ? (reg | AUTO_INC) : reg;
    return lp_core_i2c_master_write_read_device(LP_I2C_NUM_0, i2c_addr, &r, 1, buf, n, I2C_TIMEOUT_CYCLES);
}

static void on_error(esp_err_t err)
{
    err_count++;
    last_err = err;
    if (err_count == 1) {
        notify = NOTIFY_ERROR;  // 最初の 1 回だけ知らせる（あとは数えるだけ）
    }
}

// 0x08〜0x77 に、レジスタの番号（WHO_AM_I）を 1 バイト書いてみて、ACK が返ったアドレスを記録する
// （LP の読み出しの関数は NACK を見ないので、ACK を確かめる書き込みで調べる）
static void scan_bus(void)
{
    for (uint32_t a = 0x08; a < 0x78; a++) {
        uint8_t r = REG_WHO_AM_I;
        if (lp_core_i2c_master_write_to_device(LP_I2C_NUM_0, a, &r, 1, I2C_TIMEOUT_CYCLES) == ESP_OK) {
            scan_found[a / 32] |= 1u << (a % 32);
            if (i2c_addr == 0 && (a == 0x18 || a == 0x19)) {
                i2c_addr = a;  // LIS3DH は 0x18 か 0x19
            }
        }
    }
    scan_done = 1;
}

static void setup_sensor(void)
{
    if (!scan_done) {
        scan_bus();
    }
    if (i2c_addr == 0) {
        on_error(ESP_ERR_NOT_FOUND);  // LIS3DH が見つからない（次の回にもう一度スキャン）
        scan_done = 0;
        return;
    }
    uint8_t id = 0;
    esp_err_t err = read_regs(REG_WHO_AM_I, &id, 1);
    if (err != ESP_OK) {
        on_error(err);
        return;
    }
    who_am_i = id;
    if (id != 0x33) {
        on_error(ESP_FAIL);
        return;
    }
    err = write_reg(REG_CTRL1, 0x57);  // 100 Hz、X・Y・Z 有効
    if (err == ESP_OK) {
        err = write_reg(REG_CTRL4, 0x88);  // BDU=1、高分解能（12 ビット）、±2 g
    }
    if (err != ESP_OK) {
        on_error(err);
        return;
    }
    sensor_ready = 1;
}

int main(void)
{
    run_count++;

    if (!sensor_ready) {
        setup_sensor();
    } else {
        uint8_t b[6];
        esp_err_t err = read_regs(REG_OUT_X_L, b, sizeof(b));
        if (err != ESP_OK) {
            on_error(err);
        } else {
            // 左詰めの 12 ビット。±2 g の高分解能モードで 1 mg/digit
            ax_mg = (int16_t)(b[0] | (b[1] << 8)) >> 4;
            ay_mg = (int16_t)(b[2] | (b[3] << 8)) >> 4;
            az_mg = (int16_t)(b[4] | (b[5] << 8)) >> 4;

            uint32_t now = (iabs(ax_mg) > threshold_mg || iabs(ay_mg) > threshold_mg);
            if (sensor_ready == 1) {
                sensor_ready = 2;  // 最初の値: 置いたときの値として HP に知らせる
                notify = NOTIFY_READY;
            } else if (now != tilted) {
                uint32_t i = ev_n % EV_MAX;
                ev_run[i] = run_count;
                ev_x[i] = ax_mg;
                ev_y[i] = ay_mg;
                ev_z[i] = az_mg;
                ev_tilt[i] = now;
                ev_n++;
                if (now) {
                    notify = NOTIFY_TILT;  // 傾いていない → 傾いた、のときだけ起こす
                }
            }
            tilted = now;
        }
    }

    if (notify != 0) {
        // HP が起きている間の「起こす」は効かないので、HP が読むまで毎回出す
        wake_count++;
        ulp_lp_core_wakeup_main_processor();
    }
    return 0;
}
