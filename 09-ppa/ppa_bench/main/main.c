/*
 * PPA（Pixel-Processing Accelerator）と CPU で、同じ画像処理にかかる時間を比べる。
 * 画像はすべて PSRAM に置く。液晶もカメラも使わない。
 *
 * 1. 回転 90°        RGB565 1024x600 -> 600x1024
 * 2. 縮小 1/2        RGB565 1024x600 -> 512x300（双線形）
 * 3. 拡大 2 倍       RGB565 512x300  -> 1024x600（双線形）
 * 4. 色の変換        RGB888 1024x600 -> RGB565
 * 5. 色の変換        YUV422（UYVY、BT.601、limited）1024x600 -> RGB565
 * 6. 塗りつぶし      RGB565 1024x600
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/ppa.h"

#define W 1024
#define H 600
#define REPEAT 10
#define ALIGN 128  // L1・L2 のキャッシュラインに合わせる（出力バッファの決まり）

static const char *TAG = "ppa_bench";

static ppa_client_handle_t s_srm, s_fill;

static void *alloc_psram(size_t size)
{
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    void *p = heap_caps_aligned_calloc(ALIGN, 1, size, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        printf("alloc %u failed\n", (unsigned)size);
        abort();
    }
    return p;
}

/* ---------- 入力の画像を作る ---------- */

static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void make_rgb888(uint8_t *p)
{
    // PPA の RGB888 は、メモリ上で B, G, R の順（ESP_COLOR_FOURCC_BGR24）
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t r = (uint8_t)(x * 255 / (W - 1));
            uint8_t g = (uint8_t)(y * 255 / (H - 1));
            uint8_t b = (uint8_t)(((x / 64) ^ (y / 64)) & 1 ? 200 : 40);
            *p++ = b;
            *p++ = g;
            *p++ = r;
        }
    }
}

static void make_rgb565_from_888(const uint8_t *src, uint16_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        dst[i] = rgb565(src[3 * i + 2], src[3 * i + 1], src[3 * i]);
    }
}

static void make_uyvy(uint8_t *p)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x += 2) {
            uint8_t y0 = (uint8_t)(16 + (x * 219) / (W - 1));
            uint8_t y1 = (uint8_t)(16 + ((x + 1) * 219) / (W - 1));
            uint8_t u = (uint8_t)(16 + (y * 224) / (H - 1));
            uint8_t v = (uint8_t)(240 - (y * 224) / (H - 1));
            *p++ = u;
            *p++ = y0;
            *p++ = v;
            *p++ = y1;
        }
    }
}

/* ---------- CPU で同じことをする ---------- */

// 反時計回りに 90°（PPA_SRM_ROTATION_ANGLE_90 と同じ向き）
static void cpu_rotate90(const uint16_t *src, uint16_t *dst, int w, int h)
{
    // 出力は幅 h・高さ w。入力の (x, y) は出力の (y, w - 1 - x) に行く
    for (int y = 0; y < h; y++) {
        const uint16_t *s = src + y * w;
        for (int x = 0; x < w; x++) {
            dst[(w - 1 - x) * h + y] = s[x];
        }
    }
}

// 双線形の拡大縮小（16.16 の固定小数点）
static void cpu_scale_bilinear(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh)
{
    uint32_t fx = ((uint32_t)sw << 16) / dw;
    uint32_t fy = ((uint32_t)sh << 16) / dh;
    for (int y = 0; y < dh; y++) {
        uint32_t syf = y * fy;
        int y0 = syf >> 16;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        uint32_t wy = (syf >> 8) & 0xFF;
        const uint16_t *r0 = src + y0 * sw;
        const uint16_t *r1 = src + y1 * sw;
        for (int x = 0; x < dw; x++) {
            uint32_t sxf = x * fx;
            int x0 = sxf >> 16;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t wx = (sxf >> 8) & 0xFF;
            uint16_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1];
            uint32_t out = 0;
            // R(5)・G(6)・B(5) を別々に混ぜる
            static const uint32_t shift[3] = {11, 5, 0};
            static const uint32_t mask[3] = {0x1F, 0x3F, 0x1F};
            for (int c = 0; c < 3; c++) {
                uint32_t a = (p00 >> shift[c]) & mask[c], b = (p01 >> shift[c]) & mask[c];
                uint32_t cc = (p10 >> shift[c]) & mask[c], d = (p11 >> shift[c]) & mask[c];
                uint32_t top = a * (256 - wx) + b * wx;
                uint32_t bot = cc * (256 - wx) + d * wx;
                uint32_t v = (top * (256 - wy) + bot * wy + 32768) >> 16;
                out |= v << shift[c];
            }
            dst[y * dw + x] = (uint16_t)out;
        }
    }
}

static void cpu_rgb888_to_565(const uint8_t *src, uint16_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        dst[i] = rgb565(src[2], src[1], src[0]);
        src += 3;
    }
}

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

// BT.601・limited range の YUV -> RGB（整数の近似）
static void cpu_uyvy_to_565(const uint8_t *src, uint16_t *dst, int n)
{
    for (int i = 0; i < n; i += 2) {
        int u = src[0] - 128, y0 = src[1] - 16, v = src[2] - 128, y1 = src[3] - 16;
        src += 4;
        int rv = 409 * v, guv = -100 * u - 208 * v, bu = 516 * u;
        int c0 = 298 * y0, c1 = 298 * y1;
        dst[i] = rgb565(clamp8((c0 + rv + 128) >> 8), clamp8((c0 + guv + 128) >> 8), clamp8((c0 + bu + 128) >> 8));
        dst[i + 1] = rgb565(clamp8((c1 + rv + 128) >> 8), clamp8((c1 + guv + 128) >> 8), clamp8((c1 + bu + 128) >> 8));
    }
}

static void cpu_fill565(uint16_t *dst, int n, uint16_t color)
{
    uint32_t c2 = ((uint32_t)color << 16) | color;
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0; i < n / 2; i++) {
        d[i] = c2;
    }
}

/* ---------- PPA ---------- */

static esp_err_t ppa_srm(const void *in, int iw, int ih, ppa_srm_color_mode_t icm,
                         void *out, int ow, int oh, ppa_srm_color_mode_t ocm, size_t out_size,
                         ppa_srm_rotation_angle_t rot, float scale)
{
    ppa_srm_oper_config_t c = {
        .in = {
            .buffer = in, .pic_w = iw, .pic_h = ih, .block_w = iw, .block_h = ih,
            .srm_cm = icm, .yuv_range = PPA_COLOR_RANGE_LIMIT, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = out, .buffer_size = (out_size + ALIGN - 1) & ~(ALIGN - 1),
            .pic_w = ow, .pic_h = oh, .srm_cm = ocm,
        },
        .rotation_angle = rot,
        .scale_x = scale,
        .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_srm, &c);
}

static esp_err_t ppa_fill565(void *out, int w, int h, uint32_t argb)
{
    ppa_fill_oper_config_t c = {
        .out = {
            .buffer = out, .buffer_size = (size_t)w * h * 2,
            .pic_w = w, .pic_h = h, .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = w,
        .fill_block_h = h,
        .fill_argb_color = {.val = argb},
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(s_fill, &c);
}

/* ---------- 比べる ---------- */

// RGB565 の 2 枚を比べ、どれかの色が tol より大きく違う画素の数を返す
static int diff565(const uint16_t *a, const uint16_t *b, int n, int tol)
{
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int dr = (int)((a[i] >> 11) & 0x1F) - (int)((b[i] >> 11) & 0x1F);
        int dg = (int)((a[i] >> 5) & 0x3F) - (int)((b[i] >> 5) & 0x3F);
        int db = (int)(a[i] & 0x1F) - (int)(b[i] & 0x1F);
        if (dr > tol || dr < -tol || dg > tol || dg < -tol || db > tol || db < -tol) {
            bad++;
        }
    }
    return bad;
}

static void report(const char *name, int pixels, int64_t t_ppa, int64_t t_cpu, int bad0, int bad1, int n)
{
    // 時間は REPEAT 回の平均（µs）。fps は 1 秒 / 1 回の時間
    double ppa_us = (double)t_ppa / REPEAT, cpu_us = (double)t_cpu / REPEAT;
    printf("%-22s px=%7d  PPA %9.1f us (%6.1f fps, %5.1f Mpx/s)  CPU %9.1f us (%6.1f fps)  x%5.1f  diff(=0:%d, >1:%d / %d)\n",
           name, pixels, ppa_us, 1e6 / ppa_us, pixels / ppa_us, cpu_us, 1e6 / cpu_us, cpu_us / ppa_us, bad0, bad1, n);
}

#define TIME_IT(total, expr)                         \
    do {                                             \
        expr; /* 1 回目はキャッシュを温める分として捨てる */ \
        int64_t _t0 = esp_timer_get_time();          \
        for (int _i = 0; _i < REPEAT; _i++) {        \
            expr;                                    \
        }                                            \
        total = esp_timer_get_time() - _t0;          \
    } while (0)

static void bench(void *arg)
{
    const int n = W * H;
    uint8_t *src888 = alloc_psram(n * 3);
    uint16_t *src565 = alloc_psram(n * 2);
    uint8_t *srcuyvy = alloc_psram(n * 2);
    uint16_t *half = alloc_psram(n / 4 * 2);
    uint16_t *out_ppa = alloc_psram(n * 2);
    uint16_t *out_cpu = alloc_psram(n * 2);
    int64_t tp, tc;

    make_rgb888(src888);
    make_rgb565_from_888(src888, src565, n);
    make_uyvy(srcuyvy);
    printf("image %dx%d, buffers in PSRAM, repeat %d\n", W, H, REPEAT);

    // 1. 回転 90°
    TIME_IT(tp, ESP_ERROR_CHECK(ppa_srm(src565, W, H, PPA_SRM_COLOR_MODE_RGB565, out_ppa, H, W,
                                        PPA_SRM_COLOR_MODE_RGB565, n * 2, PPA_SRM_ROTATION_ANGLE_90, 1.0f)));
    TIME_IT(tc, cpu_rotate90(src565, out_cpu, W, H));
    report("rotate90 565", n, tp, tc, diff565(out_ppa, out_cpu, n, 0), diff565(out_ppa, out_cpu, n, 1), n);

    // 2. 縮小 1/2
    const int hn = (W / 2) * (H / 2);
    TIME_IT(tp, ESP_ERROR_CHECK(ppa_srm(src565, W, H, PPA_SRM_COLOR_MODE_RGB565, out_ppa, W / 2, H / 2,
                                        PPA_SRM_COLOR_MODE_RGB565, hn * 2, PPA_SRM_ROTATION_ANGLE_0, 0.5f)));
    TIME_IT(tc, cpu_scale_bilinear(src565, W, H, out_cpu, W / 2, H / 2));
    report("scale 1/2 565", n, tp, tc, diff565(out_ppa, out_cpu, hn, 0), diff565(out_ppa, out_cpu, hn, 1), hn);
    memcpy(half, out_ppa, hn * 2);

    // 3. 拡大 2 倍（2. の結果を入力に）
    TIME_IT(tp, ESP_ERROR_CHECK(ppa_srm(half, W / 2, H / 2, PPA_SRM_COLOR_MODE_RGB565, out_ppa, W, H,
                                        PPA_SRM_COLOR_MODE_RGB565, n * 2, PPA_SRM_ROTATION_ANGLE_0, 2.0f)));
    TIME_IT(tc, cpu_scale_bilinear(half, W / 2, H / 2, out_cpu, W, H));
    report("scale x2 565", n, tp, tc, diff565(out_ppa, out_cpu, n, 0), diff565(out_ppa, out_cpu, n, 1), n);

    // 4. RGB888 -> RGB565
    TIME_IT(tp, ESP_ERROR_CHECK(ppa_srm(src888, W, H, PPA_SRM_COLOR_MODE_RGB888, out_ppa, W, H,
                                        PPA_SRM_COLOR_MODE_RGB565, n * 2, PPA_SRM_ROTATION_ANGLE_0, 1.0f)));
    TIME_IT(tc, cpu_rgb888_to_565(src888, out_cpu, n));
    report("rgb888->565", n, tp, tc, diff565(out_ppa, out_cpu, n, 0), diff565(out_ppa, out_cpu, n, 1), n);

    // 5. YUV422（UYVY）-> RGB565
    TIME_IT(tp, ESP_ERROR_CHECK(ppa_srm(srcuyvy, W, H, PPA_SRM_COLOR_MODE_YUV422_UYVY, out_ppa, W, H,
                                        PPA_SRM_COLOR_MODE_RGB565, n * 2, PPA_SRM_ROTATION_ANGLE_0, 1.0f)));
    TIME_IT(tc, cpu_uyvy_to_565(srcuyvy, out_cpu, n));
    report("uyvy->565 (bt601)", n, tp, tc, diff565(out_ppa, out_cpu, n, 0), diff565(out_ppa, out_cpu, n, 1), n);

    // 6. 塗りつぶし
    TIME_IT(tp, ESP_ERROR_CHECK(ppa_fill565(out_ppa, W, H, 0xFF1E90FF)));
    TIME_IT(tc, cpu_fill565(out_cpu, n, rgb565(0x1E, 0x90, 0xFF)));
    report("fill 565", n, tp, tc, diff565(out_ppa, out_cpu, n, 0), diff565(out_ppa, out_cpu, n, 1), n);

    // 参考: 回転の結果の角の画素（向きの確認）
    ESP_ERROR_CHECK(ppa_srm(src565, W, H, PPA_SRM_COLOR_MODE_RGB565, out_ppa, H, W,
                            PPA_SRM_COLOR_MODE_RGB565, n * 2, PPA_SRM_ROTATION_ANGLE_90, 1.0f));
    printf("src(0,0)=%04x src(W-1,0)=%04x | rot(0,0)=%04x rot(0,W-1)=%04x\n",
           src565[0], src565[W - 1], out_ppa[0], out_ppa[(W - 1) * H]);

    printf("done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ppa_client_config_t srm = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_config_t fill = {.oper_type = PPA_OPERATION_FILL};
    ESP_ERROR_CHECK(ppa_register_client(&srm, &s_srm));
    ESP_ERROR_CHECK(ppa_register_client(&fill, &s_fill));
    ESP_LOGI(TAG, "start");
    // CPU の測定がほかのタスクに割り込まれにくいよう、優先度を上げて core 0 で動かす
    xTaskCreatePinnedToCore(bench, "bench", 8192, NULL, 10, NULL, 0);
}
