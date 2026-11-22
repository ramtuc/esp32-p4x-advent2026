/*
 * I2S の MEMS マイク（Adafruit I2S MEMS Microphone Breakout - ICS-43434）の音を、
 * ESP-DSP の FFT で周波数解析する。ESP32-S3 と ESP32-P4 の両方でビルドできる
 * ESP-IDF v6.0.3 / espressif/esp-dsp 1.8.2
 *
 * - I2S（標準のフィリップス形式）、48kHz、1 スロット 32 ビット、モノラル（左）
 *   ICS-43434 は 1 フレーム 64 クロック（32 ビット × 左右）で、24 ビットの 2 の補数を MSB から出す
 * - 4096 点ずつ、直流を引き、ハン窓をかけて、浮動小数点の基数 4 の FFT
 * - いちばん強い周波数の箱を探し、両隣との比で箱の間を補間して周波数を出す
 * - 1 ブロック（4096 点 = 85.3ms）ごとに、処理にかかった時間と、I2S の受信の取りこぼしの回数を出す
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
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FS          48000           /* サンプリング周波数（Hz） */
#define N           4096            /* FFT の点数 */

/* ピン（記事の配線表と同じ） */
#if CONFIG_IDF_TARGET_ESP32S3
/* ESPr Developer S3 Type-C: シルクの 6・7・8 */
#define PIN_BCLK    GPIO_NUM_6
#define PIN_WS      GPIO_NUM_7      /* マイクの LRCL */
#define PIN_DIN     GPIO_NUM_8      /* マイクの DOUT */
#elif CONFIG_IDF_TARGET_ESP32P4
/* ESP32-P4X-Function-EV-Board の J1: 11 番 GPIO21、13 番 GPIO20、12 番 GPIO22
 * （回路図で J1 だけにつながっている GPIO。J1 の GPIO26・27 は USB、53・54・6 はボードの上の部品にもつながる） */
#define PIN_BCLK    GPIO_NUM_21
#define PIN_WS      GPIO_NUM_20
#define PIN_DIN     GPIO_NUM_22
#endif

static i2s_chan_handle_t s_rx;
static int32_t *s_raw;          /* I2S から読んだ 32 ビットの値 × N */
static float   *s_fc;           /* 複素数（実部・虚部）× N */
static float   *s_win;          /* ハン窓 × N */
static volatile uint32_t s_ovf; /* 受信の取りこぼし（DMA のバッファがあふれた回数） */

static bool IRAM_ATTR on_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *arg)
{
    s_ovf++;
    return false;
}

static void i2s_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;        /* 512 フレーム × 8 = 4096 フレーム分（85.3ms）をためられる */
    cc.dma_frame_num = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &s_rx));

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_DIN,
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   /* マイクの SEL が Low（GND）なら左 */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &sc));

    i2s_event_callbacks_t cb = { .on_recv_q_ovf = on_ovf };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx, &cb, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static void mic_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));   /* P4X の J19（USB Serial/JTAG）は開き直すまでの出力が捨てられる */
    esp_chip_info_t info;
    esp_chip_info(&info);
    const uint32_t mhz = esp_rom_get_cpu_ticks_per_us();
    printf("\nchip=%s rev v%d.%d cpu=%" PRIu32 "MHz fs=%d N=%d bin=%.2fHz block=%.1fms BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d\n",
           CONFIG_IDF_TARGET, info.revision / 100, info.revision % 100, mhz, FS, N,
           (double)FS / N, 1000.0 * N / FS, PIN_BCLK, PIN_WS, PIN_DIN);

    s_raw = heap_caps_aligned_alloc(16, sizeof(int32_t) * N, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_fc  = heap_caps_aligned_alloc(16, sizeof(float) * 2 * N, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_win = heap_caps_aligned_alloc(16, sizeof(float) * N, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK((s_raw && s_fc && s_win) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(dsps_fft4r_init_fc32(NULL, N));
    dsps_wind_hann_f32(s_win, N);

    i2s_init();

    for (uint32_t blk = 0;; blk++) {
        size_t got = 0;
        ESP_ERROR_CHECK(i2s_channel_read(s_rx, s_raw, sizeof(int32_t) * N, &got, portMAX_DELAY));
        const uint32_t t0 = esp_cpu_get_cycle_count();

        /* 32 ビットの上 24 ビットが音。-1〜1 の float に直し、直流（平均）を引いて窓をかける */
        float mean = 0;
        int32_t rmin = INT32_MAX, rmax = INT32_MIN;   /* 配線の確かめ用: 読んだ値の範囲 */
        for (int i = 0; i < N; i++) {
            if (s_raw[i] < rmin) { rmin = s_raw[i]; }
            if (s_raw[i] > rmax) { rmax = s_raw[i]; }
            s_fc[2 * i] = (float)(s_raw[i] >> 8) * (1.0f / 8388608.0f);
            mean += s_fc[2 * i];
        }
        mean /= N;
        float peak_abs = 0;
        for (int i = 0; i < N; i++) {
            const float x = s_fc[2 * i] - mean;
            if (fabsf(x) > peak_abs) peak_abs = fabsf(x);
            s_fc[2 * i] = x * s_win[i];
            s_fc[2 * i + 1] = 0;
        }

        const uint32_t t1 = esp_cpu_get_cycle_count();
        dsps_fft4r_fc32(s_fc, N);
        dsps_bit_rev4r_fc32(s_fc, N);
        const uint32_t t2 = esp_cpu_get_cycle_count();

        /* 大きさ（2 乗）。箱 0（直流）は飛ばし、N/2 まで探す */
        int k = 1;
        float best = -1;
        for (int i = 1; i < N / 2; i++) {
            const float p = s_fc[2 * i] * s_fc[2 * i] + s_fc[2 * i + 1] * s_fc[2 * i + 1];
            s_fc[i] = p;                  /* 前から詰めて置き直す（i < 2i なので壊さない） */
            if (p > best) { best = p; k = i; }
        }
        /* 両隣との比（対数）で、山の頂上を放物線で補間 */
        float delta = 0;
        if (k > 1 && k < N / 2 - 1) {
            const float a = logf(s_fc[k - 1] + 1e-30f), b = logf(s_fc[k] + 1e-30f), c = logf(s_fc[k + 1] + 1e-30f);
            const float den = a - 2 * b + c;
            if (den != 0) delta = 0.5f * (a - c) / den;
        }
        const uint32_t t3 = esp_cpu_get_cycle_count();

        printf("blk=%" PRIu32 " peak=%.1fHz bin=%d level=%.1fdBFS proc=%" PRIu32 "us fft=%" PRIu32 "us block=%dus ovf=%" PRIu32 " raw=%" PRId32 "..%" PRId32 "\n",
               blk, (k + delta) * FS / (float)N, k,
               20.0f * log10f(peak_abs + 1e-12f),
               (t3 - t0) / mhz, (t2 - t1) / mhz, (int)(1000000LL * N / FS), s_ovf,
               rmin >> 8, rmax >> 8);
    }
}

void app_main(void)
{
    xTaskCreatePinnedToCore(mic_task, "mic", 8192, NULL, 5, NULL, 0);
}
