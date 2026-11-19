/*
 * ESP-DSP の FFT のサイクル数を ESP32-S3 と ESP32-P4 で比べる
 * ESP-IDF v6.0.3 / espressif/esp-dsp 1.8.2
 *
 * - 入力は合成データ（sin 波。N 点のうち、ちょうど N/16 番目の周波数の箱に乗る周波数）
 * - 1024 点と 4096 点
 * - 浮動小数点（fc32）の基数 2・基数 4、16 ビット固定小数点（sc16）の基数 2
 * - それぞれ C で書いた版（_ansi）と、チップ向けに最適化した版（S3 は _aes3、P4 は _arp4）
 * - 計るのは FFT の本体（バタフライ）だけ。並べ替え（ビット反転）は別に計る
 * - 10 回回して最小のサイクル数を出す（キャッシュに乗った状態の値）
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "esp_dsp.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REPEAT  10
#define XSTR(x) STR(x)
#define STR(x)  #x

static float   *s_fc;   /* 複素数（実部・虚部の順）× N */
static int16_t *s_sc;
static uint32_t s_mhz;

static void fill_fc32(int n)
{
    const int k = n / 16;   /* 周波数の箱の番号 */
    for (int i = 0; i < n; i++) {
        s_fc[2 * i]     = sinf(2.0f * (float)M_PI * k * i / n);
        s_fc[2 * i + 1] = 0.0f;
    }
}

static void fill_sc16(int n)
{
    const int k = n / 16;
    for (int i = 0; i < n; i++) {
        s_sc[2 * i]     = (int16_t)(16000.0f * sinf(2.0f * (float)M_PI * k * i / n));
        s_sc[2 * i + 1] = 0;
    }
}

/* 前半（0〜N/2-1）で大きさが最大の箱を返す（並べ替えのあとに呼ぶ） */
static int peak_fc32(int n)
{
    int best = 0; float bv = -1;
    for (int i = 0; i < n / 2; i++) {
        float v = s_fc[2 * i] * s_fc[2 * i] + s_fc[2 * i + 1] * s_fc[2 * i + 1];
        if (v > bv) { bv = v; best = i; }
    }
    return best;
}

static int peak_sc16(int n)
{
    int best = 0; int32_t bv = -1;
    for (int i = 0; i < n / 2; i++) {
        int32_t v = (int32_t)s_sc[2 * i] * s_sc[2 * i] + (int32_t)s_sc[2 * i + 1] * s_sc[2 * i + 1];
        if (v > bv) { bv = v; best = i; }
    }
    return best;
}

/* call を REPEAT 回、毎回 fill で入力を作り直してから計り、最小のサイクル数を result に入れる */
#define MEASURE(result, fill, call)                         \
    do {                                                    \
        uint32_t best_ = UINT32_MAX;                        \
        for (int r_ = 0; r_ < REPEAT; r_++) {               \
            fill;                                           \
            uint32_t t0_ = esp_cpu_get_cycle_count();       \
            call;                                           \
            uint32_t t1_ = esp_cpu_get_cycle_count();       \
            if (t1_ - t0_ < best_) best_ = t1_ - t0_;       \
        }                                                   \
        (result) = best_;                                   \
    } while (0)

static void report(const char *name, int n, uint32_t cycles, int peak)
{
    printf("%-26s N=%4d %9" PRIu32 " cycles %8.1f us", name, n, cycles, (double)cycles / s_mhz);
    if (peak >= 0) {
        printf("  peak bin=%d (expect %d)", peak, n / 16);
    }
    printf("\n");
}

static void bench(int n)
{
    uint32_t c;

    /* --- fc32 基数 2 --- */
    MEASURE(c, fill_fc32(n), dsps_fft2r_fc32_ansi(s_fc, n));
    dsps_bit_rev_fc32(s_fc, n);
    report("fft2r_fc32_ansi", n, c, peak_fc32(n));

    MEASURE(c, fill_fc32(n), dsps_fft2r_fc32(s_fc, n));
    dsps_bit_rev_fc32(s_fc, n);
    report(XSTR(dsps_fft2r_fc32), n, c, peak_fc32(n));

    MEASURE(c, fill_fc32(n), dsps_bit_rev_fc32(s_fc, n));
    report("dsps_bit_rev_fc32", n, c, -1);       /* 並べ替え（S3・P4 とも C の版） */

    /* --- fc32 基数 4 --- */
    MEASURE(c, fill_fc32(n), dsps_fft4r_fc32_ansi(s_fc, n));
    dsps_bit_rev4r_fc32(s_fc, n);
    report("fft4r_fc32_ansi", n, c, peak_fc32(n));

    MEASURE(c, fill_fc32(n), dsps_fft4r_fc32(s_fc, n));
    dsps_bit_rev4r_fc32(s_fc, n);
    report(XSTR(dsps_fft4r_fc32), n, c, peak_fc32(n));

    MEASURE(c, fill_fc32(n), dsps_bit_rev4r_fc32(s_fc, n));
    report("dsps_bit_rev4r_fc32", n, c, -1);     /* 並べ替え（表を引く。S3 は表引きが _aes3） */

    /* --- sc16 基数 2 --- */
    MEASURE(c, fill_sc16(n), dsps_fft2r_sc16_ansi(s_sc, n));
    dsps_bit_rev_sc16_ansi(s_sc, n);
    report("fft2r_sc16_ansi", n, c, peak_sc16(n));

    MEASURE(c, fill_sc16(n), dsps_fft2r_sc16(s_sc, n));
    dsps_bit_rev_sc16_ansi(s_sc, n);
    report(XSTR(dsps_fft2r_sc16), n, c, peak_sc16(n));
}

static void bench_task(void *arg)
{
    const int max_n = CONFIG_DSP_MAX_FFT_SIZE;
    /* USB Serial/JTAG（P4X の J19）は、PC が開き直すまでの出力が捨てられるので、少し待ってから始める */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_chip_info_t info;
    esp_chip_info(&info);
    s_mhz = esp_clk_cpu_freq() / 1000000;
    printf("\nchip=%s rev v%d.%d cores=%d cpu=%" PRIu32 "MHz esp-dsp max FFT=%d\n",
           CONFIG_IDF_TARGET, info.revision / 100, info.revision % 100, info.cores, s_mhz, max_n);

    /* SIMD の命令は 16 バイト境界のデータを前提にするので、内部 RAM に 16 バイト境界で取る */
    s_fc = heap_caps_aligned_alloc(16, sizeof(float) * 2 * max_n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_sc = heap_caps_aligned_alloc(16, sizeof(int16_t) * 2 * max_n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK((s_fc && s_sc) ? ESP_OK : ESP_ERR_NO_MEM);

    /* 回転因子（twiddle）の表を作る。NULL を渡すとライブラリが確保する */
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, max_n));
    ESP_ERROR_CHECK(dsps_fft4r_init_fc32(NULL, max_n));
    ESP_ERROR_CHECK(dsps_fft2r_init_sc16(NULL, max_n));

    bench(1024);
    bench(4096);

    printf("done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* 計っている間にほかのタスクに割り込まれにくいよう、高い優先度でコア 0 に固定 */
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, configMAX_PRIORITIES - 2, NULL, 0);
}
