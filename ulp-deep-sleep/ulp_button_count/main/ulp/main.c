/*
 * ULP RISC-V 側のプログラム（メインの CPU が Deep Sleep の間に動く）
 *
 * ULP タイマで 20ms ごとに起こされ、1 回ぶん見たら止まる（halt）。
 * GPIO0（FLASH スイッチ）が High→Low になった回数を数え、
 * WAKE_PRESSES 回に達したらメインの CPU を起こす。
 *
 * ここの大域変数は RTC SLOW メモリに置かれ、メインの CPU から
 * ulp_<名前> で読み書きできる。
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include "ulp_riscv.h"
#include "ulp_riscv_utils.h"
#include "ulp_riscv_gpio.h"

#define BUTTON_GPIO   GPIO_NUM_0

/* メインの CPU と共有する変数 */
uint32_t run_count;        /* ULP が起こされた回数 */
uint32_t press_count;      /* 押された回数（起こした後にメイン側で 0 に戻す） */
uint32_t wake_presses = 3; /* 何回押されたらメインを起こすか（メイン側で書き換え可） */
uint32_t last_level = 1;   /* 前回のレベル（離している=High） */
uint32_t low_samples;      /* Low を読んだ回数（確かめる用） */

int main(void)
{
    run_count++;

    uint32_t level = (uint32_t)ulp_riscv_gpio_get_level(BUTTON_GPIO);

    /* High→Low を 1 回と数える。20ms おきに見るので、短いチャタリングは拾いにくい */
    if (last_level == 1 && level == 0) {
        press_count++;
        if (press_count >= wake_presses) {
            ulp_riscv_wakeup_main_processor();
        }
    }
    if (level == 0) {
        low_samples++;
    }
    last_level = level;

    /* main から戻ると ulp_riscv_halt() が呼ばれ、次の ULP タイマまで止まる */
    return 0;
}
