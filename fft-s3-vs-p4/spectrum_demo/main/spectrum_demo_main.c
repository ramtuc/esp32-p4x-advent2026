/*
 * 疑似信号のスペクトルを ESP-DSP で計算する（ESP32-S3 と ESP32-P4）
 * ESP-IDF v6.0.3 / espressif/esp-dsp 1.8.2
 *
 * - サンプリング周波数 48kHz の 16 ビット PCM（マイクや ADC から取った音の代わり）をボードの上で作る
 *   440Hz・1000Hz・3300Hz（大きさが違う）と、40Hz だけ離れた 5000Hz・5040Hz の組に、小さな雑音を足す
 * - 1 フレーム（N = 1024 / 4096 点）ごとに
 *   PCM を作業用の配列へ → ハン窓 → FFT → 並べ替え → 大きさを dB に
 *   を fc32（浮動小数点、基数 4）と sc16（16 ビット固定小数点、基数 2）で計算する
 * - 段ごとのサイクル数（10 回のうちの最小）、スペクトル（dB）、入れた周波数ごとのピーク、雑音の床を出す
 * - dB は、フルスケールの sin 波（振幅 1.0）が 0dB になるように合わせる（dBFS）
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
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

#define FS       48000
#define NMAX     4096
#define REPEAT   10
#define NOISE    1e-4f          /* 雑音の大きさ（一様乱数 ±1e-4。フルスケールの -80dB） */

typedef struct { float f, a; } tone_t;
static const tone_t TONES[] = {
    {440.0f, 0.40f},    /* 箱の真ん中にない周波数（1024 点で 9.39 番目、4096 点で 37.5 番目） */
    {1000.0f, 0.20f},
    {3300.0f, 0.05f},
    {5000.0f, 0.10f},   /* 近い 2 つの組（40Hz 差）: 1024 点の箱の幅 46.9Hz より狭く、4096 点の 11.7Hz より広い */
    {5040.0f, 0.10f},
};
#define NTONES (sizeof(TONES) / sizeof(TONES[0]))

static int16_t *s_pcm;      /* 入力の PCM（Q15） */
static float   *s_fc;       /* fc32 の作業用（複素数 × N） */
static int16_t *s_sc;       /* sc16 の作業用（複素数 × N） */
static float   *s_winf;     /* ハン窓（float） */
static int16_t *s_wins;     /* ハン窓（Q15） */
static float   *s_db;       /* 大きさ（dB）× (N/2 + 1) */
static float   *s_tmp;      /* 中央値を求めるための写し */
static uint32_t s_mhz;

static uint32_t s_rng = 12345;
static float frand(void)    /* -1〜1 の一様な乱数（xorshift） */
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) / 8388608.0f - 1.0f;
}

static void make_pcm(void)
{
    for (int i = 0; i < NMAX; i++) {
        float x = 0;
        for (int t = 0; t < NTONES; t++) {
            x += TONES[t].a * sinf(2.0f * (float)M_PI * TONES[t].f * i / FS);
        }
        x += NOISE * frand();
        s_pcm[i] = (int16_t)lrintf(x * 32767.0f);
    }
}

typedef struct { uint32_t conv, win, fft, rev, db; } stage_t;

static void keep_min(stage_t *best, const stage_t *c)
{
    if (c->conv < best->conv) best->conv = c->conv;
    if (c->win < best->win) best->win = c->win;
    if (c->fft < best->fft) best->fft = c->fft;
    if (c->rev < best->rev) best->rev = c->rev;
    if (c->db < best->db) best->db = c->db;
}

/* fc32: float に直す → 窓 → 基数 4 の FFT → 並べ替え → dB */
static void run_fc32(int n, stage_t *st)
{
    const float off = 20.0f * log10f(4.0f / n);   /* ハン窓の sin 波の箱の大きさは A × N/4 */
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < n; i++) {
        s_fc[2 * i] = s_pcm[i] * (1.0f / 32768.0f);
        s_fc[2 * i + 1] = 0;
    }
    uint32_t t1 = esp_cpu_get_cycle_count();
    dsps_mul_f32(s_fc, s_winf, s_fc, n, 2, 1, 2);
    uint32_t t2 = esp_cpu_get_cycle_count();
    dsps_fft4r_fc32(s_fc, n);
    uint32_t t3 = esp_cpu_get_cycle_count();
    dsps_bit_rev4r_fc32(s_fc, n);
    uint32_t t4 = esp_cpu_get_cycle_count();
    for (int k = 0; k <= n / 2; k++) {
        const float p = s_fc[2 * k] * s_fc[2 * k] + s_fc[2 * k + 1] * s_fc[2 * k + 1];
        s_db[k] = 10.0f * log10f(p + 1e-30f) + off;
    }
    uint32_t t5 = esp_cpu_get_cycle_count();
    *st = (stage_t){t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4};
}

/* sc16: 写す → 窓（Q15 の掛け算）→ 基数 2 の FFT → 並べ替え → dB */
static void run_sc16(int n, stage_t *st)
{
    /* sc16 の FFT は段ごとに 1/2 にするので出力は 1/N。sin 波の箱の大きさは A × 32767 / 4 */
    const float off = 20.0f * log10f(4.0f / 32767.0f);
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < n; i++) {
        s_sc[2 * i] = s_pcm[i];
        s_sc[2 * i + 1] = 0;
    }
    uint32_t t1 = esp_cpu_get_cycle_count();
    dsps_mul_s16(s_sc, s_wins, s_sc, n, 2, 1, 2, 15);
    uint32_t t2 = esp_cpu_get_cycle_count();
    dsps_fft2r_sc16(s_sc, n);
    uint32_t t3 = esp_cpu_get_cycle_count();
    dsps_bit_rev_sc16_ansi(s_sc, n);
    uint32_t t4 = esp_cpu_get_cycle_count();
    for (int k = 0; k <= n / 2; k++) {
        const int32_t re = s_sc[2 * k], im = s_sc[2 * k + 1];
        const float p = (float)(re * re + im * im);
        s_db[k] = 10.0f * log10f(p + 0.25f) + off;     /* 0 のときは 1/2 LSB とみなす */
    }
    uint32_t t5 = esp_cpu_get_cycle_count();
    *st = (stage_t){t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4};
}

static int cmpf(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void report(int n, const char *type, const stage_t *b)
{
    const float df = (float)FS / n;
    const uint32_t total = b->conv + b->win + b->fft + b->rev + b->db;
    const double frame_us = 1e6 * n / FS;
    printf("TIME N=%d type=%s conv=%" PRIu32 " win=%" PRIu32 " fft=%" PRIu32 " rev=%" PRIu32 " db=%" PRIu32
           " total=%" PRIu32 "cyc total_us=%.1f frame_us=%.1f load=%.2f%%\n",
           n, type, b->conv, b->win, b->fft, b->rev, b->db, total,
           (double)total / s_mhz, frame_us, 100.0 * total / s_mhz / frame_us);

    /* スペクトル（0〜N/2 の箱、dB、0.1 刻み） */
    for (int k = 0; k <= n / 2; k += 32) {
        printf("SPEC N=%d type=%s k=%d:", n, type, k);
        for (int j = k; j < k + 32 && j <= n / 2; j++) printf(" %.1f", s_db[j]);
        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(20));  /* シリアルが追いつくように少し待つ（計る処理の外） */
    }

    /* 入れた周波数ごとに、近く（±1 箱）でいちばん大きい箱を探し、両隣との放物線で頂上の周波数を補間 */
    for (int t = 0; t < NTONES; t++) {
        const int k0 = (int)lrintf(TONES[t].f / df);
        int kb = k0;
        for (int k = k0 - 1; k <= k0 + 1; k++) if (k > 0 && k < n / 2 && s_db[k] > s_db[kb]) kb = k;
        const float a = s_db[kb - 1], c = s_db[kb], d = s_db[kb + 1];
        const float den = a - 2 * c + d;
        const float delta = den != 0 ? 0.5f * (a - d) / den : 0;
        const float fpk = (kb + delta) * df;
        printf("PEAK N=%d type=%s in=%.0fHz in_dB=%.1f bin=%d peak=%.1fHz diff=%+.1fHz level=%.1fdB\n",
               n, type, TONES[t].f, 20.0f * log10f(TONES[t].a), kb, fpk, fpk - TONES[t].f, c);
    }

    /* 近い 2 つ（5000Hz・5040Hz）の間の谷: 2 つの箱の間でいちばん小さい値と、2 つの山の低い方との差 */
    const int k1 = (int)lrintf(5000.0f / df), k2 = (int)lrintf(5040.0f / df);
    if (k2 - k1 >= 2) {
        float vmin = 1e9f;
        for (int k = k1 + 1; k < k2; k++) if (s_db[k] < vmin) vmin = s_db[k];
        const float lo = fminf(s_db[k1], s_db[k2]);
        printf("PAIR N=%d type=%s bins=%d,%d dip=%.1fdB\n", n, type, k1, k2, lo - vmin);
    } else {
        printf("PAIR N=%d type=%s bins=%d,%d dip=n/a (same or adjacent bin)\n", n, type, k1, k2);
    }

    /* 雑音の床: 全部の箱の dB の中央値（入れた周波数の山はわずかなので、中央値はほぼ雑音） */
    memcpy(s_tmp, s_db, sizeof(float) * (n / 2 + 1));
    qsort(s_tmp, n / 2 + 1, sizeof(float), cmpf);
    printf("FLOOR N=%d type=%s median=%.1fdB p90=%.1fdB\n", n, type, s_tmp[(n / 2 + 1) / 2], s_tmp[(n / 2 + 1) * 9 / 10]);
}

static void bench(int n)
{
    /* 窓は N ごとに作り直す */
    dsps_wind_hann_f32(s_winf, n);
    for (int i = 0; i < n; i++) s_wins[i] = (int16_t)lrintf(s_winf[i] * 32767.0f);

    stage_t best, cur;
    best = (stage_t){UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (int r = 0; r < REPEAT; r++) { run_fc32(n, &cur); keep_min(&best, &cur); }
    report(n, "fc32", &best);

    best = (stage_t){UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (int r = 0; r < REPEAT; r++) { run_sc16(n, &cur); keep_min(&best, &cur); }
    report(n, "sc16", &best);
}

static void task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));    /* P4X の J19（USB Serial/JTAG）は、開き直すまでの出力が捨てられる */
    esp_chip_info_t info;
    esp_chip_info(&info);
    s_mhz = esp_clk_cpu_freq() / 1000000;
    printf("\nMETA chip=%s rev=v%d.%d cpu=%" PRIu32 "MHz fs=%d noise=%g\n",
           CONFIG_IDF_TARGET, info.revision / 100, info.revision % 100, s_mhz, FS, (double)NOISE);
    for (int t = 0; t < NTONES; t++) printf("TONE f=%.0f a=%.2f\n", TONES[t].f, TONES[t].a);

    const uint32_t cap = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_pcm  = heap_caps_aligned_alloc(16, sizeof(int16_t) * NMAX, cap);
    s_fc   = heap_caps_aligned_alloc(16, sizeof(float) * 2 * NMAX, cap);
    s_sc   = heap_caps_aligned_alloc(16, sizeof(int16_t) * 2 * NMAX, cap);
    s_winf = heap_caps_aligned_alloc(16, sizeof(float) * NMAX, cap);
    s_wins = heap_caps_aligned_alloc(16, sizeof(int16_t) * NMAX, cap);
    s_db   = heap_caps_aligned_alloc(16, sizeof(float) * (NMAX / 2 + 1), cap);
    s_tmp  = heap_caps_aligned_alloc(16, sizeof(float) * (NMAX / 2 + 1), cap);
    ESP_ERROR_CHECK((s_pcm && s_fc && s_sc && s_winf && s_wins && s_db && s_tmp) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(dsps_fft4r_init_fc32(NULL, NMAX));
    ESP_ERROR_CHECK(dsps_fft2r_init_sc16(NULL, NMAX));

    make_pcm();
    /* 入力の時間波形（はじめの 10ms = 480 点） */
    for (int i = 0; i < 480; i += 40) {
        printf("WAVE i=%d:", i);
        for (int j = i; j < i + 40; j++) printf(" %d", s_pcm[j]);
        printf("\n");
    }

    bench(1024);
    bench(4096);
    printf("DONE\n");
    for (int i = 0;; i++) {             /* 動いていることが分かるように */
        printf("ALIVE %d\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    xTaskCreatePinnedToCore(task, "spec", 8192, NULL, configMAX_PRIORITIES - 2, NULL, 0);
}
