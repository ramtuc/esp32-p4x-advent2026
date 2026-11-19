// LP コアのプログラム（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// LP タイマで 100ms ごとに起こされて 1 回動き、wake_every 回ごとに HP コアを起こす
// ここで定義した変数は、HP 側から ulp_<名前> で読み書きできる（LP SRAM に置かれる）
#include <stdint.h>
#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"

volatile uint32_t run_count;        // LP コアが動いた回数（HP が読む）
volatile uint32_t wake_count;       // HP を起こした回数（HP が読む）
volatile uint32_t wake_every = 50;  // 何回ごとに HP を起こすか（HP が書き換える）

int main(void)
{
    run_count++;
    if (wake_every != 0 && (run_count % wake_every) == 0) {
        wake_count++;
        ulp_lp_core_wakeup_main_processor();
    }
    return 0;  // main から戻ると LP コアは止まり、次の LP タイマで また最初から動く
}
