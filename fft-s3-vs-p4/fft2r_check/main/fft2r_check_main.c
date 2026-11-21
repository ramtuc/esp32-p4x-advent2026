/*
 * ESP-DSP の dsps_fft2r_fc32（P4 では _arp4）N=1024 のサイクル数が、
 * 公表値（ESP-DSP Benchmarks）と合わない理由を調べるための試験
 * ESP-IDF v6.0.3 / espressif/esp-dsp 1.8.2
 *
 * 条件を 1 つずつ変えて計る
 *  A. fft_bench と同じ: 毎回 sin 波を作り直し、10 回の最小
 *  B. A に割り込みの禁止を足す
 *  C. 公表値の測り方（esp-dsp の test/report.inc の REPORT_BENCHMARK_CSV）:
 *     2 回ならしてから、割り込みを止めて 1 回だけ計る。データは作り直さない
 *  D. データの先頭を 64 バイト境界から 0/16/32/48 バイトずらす
 *  E. 回転因子の表を自分で確保（64 バイト境界から 0/16/32/48 バイトずらす）
 *  F. CPU のクロックを途中で変える（P4: 400/200/100MHz）
 *  G. 入力の値を変える（全部 0、虚部も含めて乱数、全部 1、sin 波の振幅を変える、全部 NaN）
 *     公表値の測り方では入力のバッファを初期化しないので、中身は決まっていない
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
#include "esp_rom_sys.h"
#include "esp_private/rtc_clk.h"
#include "soc/rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define N       1024
#define MAXN    CONFIG_DSP_MAX_FFT_SIZE
#define REPEAT  10

static uint8_t *s_raw;      /* データの元の領域（64 バイト境界 + 64） */
static float   *s_fc;
static uint8_t *s_wraw;     /* 回転因子の表の元の領域 */

static void fill(float *d)
{
    const int k = N / 16;
    for (int i = 0; i < N; i++) {
        d[2 * i]     = sinf(2.0f * (float)M_PI * k * i / N);
        d[2 * i + 1] = 0.0f;
    }
}

/* G 用: 入力の作り方 */
static uint32_t s_rng = 1;
static float frand(void)   /* -1〜1 の一様な乱数（xorshift） */
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) / 8388608.0f - 1.0f;
}
static void fill_pat(float *d, int pat)
{
    for (int i = 0; i < 2 * N; i++) {
        switch (pat) {
        case 0: d[i] = 0.0f; break;
        case 1: d[i] = frand(); break;
        case 2: d[i] = 1.0f; break;
        case 3: d[i] = (i & 1) ? 0.0f : 1e-3f * sinf(2.0f * (float)M_PI * (N / 16) * (i / 2) / N); break;
        case 4: d[i] = (i & 1) ? 0.0f : 1e3f * sinf(2.0f * (float)M_PI * (N / 16) * (i / 2) / N); break;
        default: d[i] = NAN; break;
        }
    }
}
static const char *const s_pat_name[] = {"zeros", "random re+im", "all 1.0", "sin x1e-3", "sin x1e3", "all NaN"};

typedef esp_err_t (*fft_fn)(float *, int, float *);

static uint32_t run_once(fft_fn f, float *d, int masked)
{
    unsigned lv = 0;
    if (masked) lv = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t t0 = esp_cpu_get_cycle_count();
    f(d, N, dsps_fft_w_table_fc32);
    uint32_t t1 = esp_cpu_get_cycle_count();
    if (masked) portCLEAR_INTERRUPT_MASK_FROM_ISR(lv);
    return t1 - t0;
}

/* A・B: 毎回作り直して REPEAT 回の最小 */
static uint32_t min_refill(fft_fn f, float *d, int masked)
{
    uint32_t best = UINT32_MAX;
    for (int r = 0; r < REPEAT; r++) {
        fill(d);
        uint32_t c = run_once(f, d, masked);
        if (c < best) best = c;
    }
    return best;
}

/* C: REPORT_BENCHMARK_CSV と同じ順（ならし 1 回 → 割り込み禁止 → ならし 1 回 → 計る） */
static uint32_t bm_style(fft_fn f, float *d)
{
    f(d, N, dsps_fft_w_table_fc32);
    unsigned lv = portSET_INTERRUPT_MASK_FROM_ISR();
    f(d, N, dsps_fft_w_table_fc32);
    uint32_t t0 = esp_cpu_get_cycle_count();
    f(d, N, dsps_fft_w_table_fc32);
    uint32_t t1 = esp_cpu_get_cycle_count();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(lv);
    return t1 - t0;
}

#if CONFIG_IDF_TARGET_ESP32P4
#define OPT_FN   dsps_fft2r_fc32_arp4_
#define OPT_NAME "dsps_fft2r_fc32_arp4"
#elif CONFIG_IDF_TARGET_ESP32S3
#define OPT_FN   dsps_fft2r_fc32_aes3_
#define OPT_NAME "dsps_fft2r_fc32_aes3"
#endif

static void line(const char *cond, uint32_t opt, uint32_t ansi)
{
    printf("%-34s %-22s %8" PRIu32 "   ansi %8" PRIu32 "\n", cond, OPT_NAME, opt, ansi);
}

static void all_conditions(const char *tag)
{
    char buf[48];
    float *d = s_fc;

    snprintf(buf, sizeof buf, "%s A refill min10", tag);
    line(buf, min_refill(OPT_FN, d, 0), min_refill(dsps_fft2r_fc32_ansi_, d, 0));
    snprintf(buf, sizeof buf, "%s B refill min10 masked", tag);
    line(buf, min_refill(OPT_FN, d, 1), min_refill(dsps_fft2r_fc32_ansi_, d, 1));
    fill(d);
    snprintf(buf, sizeof buf, "%s C bm-style (sin first)", tag);
    uint32_t o = bm_style(OPT_FN, d);
    uint32_t a = bm_style(dsps_fft2r_fc32_ansi_, d);
    line(buf, o, a);
    memset(d, 0, sizeof(float) * 2 * N);
    snprintf(buf, sizeof buf, "%s C bm-style (zeros)", tag);
    o = bm_style(OPT_FN, d);
    a = bm_style(dsps_fft2r_fc32_ansi_, d);
    line(buf, o, a);
}

static void bench_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));   /* P4X の J19（USB Serial/JTAG）は開き直すまでの出力が捨てられる */
    esp_chip_info_t info;
    esp_chip_info(&info);
    printf("\nchip=%s rev v%d.%d cpu=%" PRIu32 "MHz esp-dsp max FFT=%d N=%d\n",
           CONFIG_IDF_TARGET, info.revision / 100, info.revision % 100,
           esp_rom_get_cpu_ticks_per_us(), MAXN, N);

    s_raw = heap_caps_aligned_alloc(64, sizeof(float) * 2 * N + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_wraw = heap_caps_aligned_alloc(64, sizeof(float) * MAXN + 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK((s_raw && s_wraw) ? ESP_OK : ESP_ERR_NO_MEM);
    s_fc = (float *)s_raw;

    /* ライブラリに表を確保させる（fft_bench と同じ） */
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, MAXN));
    printf("data=%p table=%p (library alloc)\n", (void *)s_fc, (void *)dsps_fft_w_table_fc32);
    all_conditions("400");

    /* D: データの先頭をずらす */
    for (int off = 0; off < 64; off += 16) {
        float *d = (float *)(s_raw + off);
        char buf[48];
        snprintf(buf, sizeof buf, "D data offset %2d", off);
        line(buf, min_refill(OPT_FN, d, 1), min_refill(dsps_fft2r_fc32_ansi_, d, 1));
    }

    /* E: 表を自分の領域に作り直す */
    for (int off = 0; off < 64; off += 16) {
        dsps_fft2r_deinit_fc32();
        ESP_ERROR_CHECK(dsps_fft2r_init_fc32((float *)(s_wraw + off), MAXN));
        char buf[48];
        snprintf(buf, sizeof buf, "E table offset %2d", off);
        line(buf, min_refill(OPT_FN, s_fc, 1), min_refill(dsps_fft2r_fc32_ansi_, s_fc, 1));
    }
    dsps_fft2r_deinit_fc32();
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, MAXN));

    /* G: 入力の値を変える（毎回作り直して 10 回の最小、割り込み禁止） */
    for (int pat = 0; pat < 6; pat++) {
        uint32_t bo = UINT32_MAX, ba = UINT32_MAX;
        for (int r = 0; r < REPEAT; r++) {
            fill_pat(s_fc, pat); uint32_t c = run_once(OPT_FN, s_fc, 1); if (c < bo) bo = c;
            fill_pat(s_fc, pat); c = run_once(dsps_fft2r_fc32_ansi_, s_fc, 1); if (c < ba) ba = c;
        }
        char buf[48];
        snprintf(buf, sizeof buf, "G %s", s_pat_name[pat]);
        line(buf, bo, ba);
    }

#if CONFIG_IDF_TARGET_ESP32P4
    /* F: クロックを変える（rev v3 の ESP-IDF v6.0.3 で選べる 400/200/100MHz） */
    static const int mhz[] = {200, 100, 400};
    for (int i = 0; i < 3; i++) {
        rtc_cpu_freq_config_t cfg;
        if (!rtc_clk_cpu_freq_mhz_to_config(mhz[i], &cfg)) {
            printf("F %d MHz: not supported\n", mhz[i]);
            continue;
        }
        rtc_clk_cpu_freq_set_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(100));
        char tag[8];
        snprintf(tag, sizeof tag, "%d", mhz[i]);
        printf("F cpu=%" PRIu32 "MHz\n", esp_rom_get_cpu_ticks_per_us());
        all_conditions(tag);
    }
#endif
    printf("done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, configMAX_PRIORITIES - 2, NULL, 0);
}
