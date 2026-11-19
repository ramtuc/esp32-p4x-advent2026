// PSRAM の読み書きの速さを、CPU（キャッシュ経由）と DMA（AXI の GDMA）で測る（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// 速さは MB/s（1MB = 1,000,000 バイト）。PSRAM の速さ（200MHz / 250MHz）は sdkconfig で切り替える
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_async_memcpy.h"
#include "sdkconfig.h"

#define PSRAM_BUF  (4 * 1024 * 1024)  // PSRAM の領域（L2 キャッシュよりずっと大きい）
#define INT_BUF    (128 * 1024)       // 内蔵 RAM の領域
#define SMALL_BUF  (32 * 1024)        // L1 データキャッシュ（64KB）に収まる大きさ
#define TOTAL      (16 * 1024 * 1024) // どの測り方でも、合わせてこれだけ動かす

static uint8_t *s_ps_a, *s_ps_b, *s_int_a, *s_int_b;

static void report(const char *name, size_t bytes, int64_t us)
{
    printf("%-34s %8.1f MB/s\n", name, (double)bytes / (double)us);
}

// ---- CPU ----
static void cpu_read(const char *name, const uint8_t *buf, size_t size)
{
    volatile uint32_t sink;
    uint32_t sum = 0;
    int64_t t0 = esp_timer_get_time();
    for (size_t done = 0; done < TOTAL; done += size) {
        const uint32_t *p = (const uint32_t *)buf;
        for (size_t i = 0; i < size / 4; i += 4) {
            sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
        }
    }
    int64_t t1 = esp_timer_get_time();
    sink = sum;
    (void)sink;
    report(name, TOTAL, t1 - t0);
}

static void cpu_write(const char *name, uint8_t *buf, size_t size, bool writeback)
{
    int64_t t0 = esp_timer_get_time();
    for (size_t done = 0; done < TOTAL; done += size) {
        uint32_t *p = (uint32_t *)buf;
        uint32_t v = (uint32_t)done;
        for (size_t i = 0; i < size / 4; i += 4) {
            p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        }
        if (writeback) {
            // キャッシュに残った分を PSRAM へ書き戻すまでを含める
            ESP_ERROR_CHECK(esp_cache_msync(buf, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
        }
    }
    int64_t t1 = esp_timer_get_time();
    report(name, TOTAL, t1 - t0);
}

// dst_step / src_step が true なら、その側は 4MB の中を size ずつずらしていく
// （PSRAM の同じ所をくり返し使うと L2 キャッシュに当たって、PSRAM の速さにならないため）
static void cpu_memcpy(const char *name, uint8_t *dst, bool dst_step, const uint8_t *src, bool src_step, size_t size)
{
    int64_t t0 = esp_timer_get_time();
    for (size_t done = 0; done < TOTAL; done += size) {
        size_t off = done % PSRAM_BUF;
        memcpy(dst + (dst_step ? off : 0), src + (src_step ? off : 0), size);
    }
    int64_t t1 = esp_timer_get_time();
    report(name, TOTAL, t1 - t0);
}

// ---- DMA（AXI の GDMA） ----
static SemaphoreHandle_t s_done;

static bool on_done(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static void dma_copy(async_memcpy_handle_t mcp, const char *name, uint8_t *dst, bool dst_step, uint8_t *src, bool src_step, size_t size)
{
    int64_t t0 = esp_timer_get_time();
    for (size_t done = 0; done < TOTAL; done += size) {
        size_t off = done % PSRAM_BUF;
        ESP_ERROR_CHECK(esp_async_memcpy(mcp, dst + (dst_step ? off : 0), src + (src_step ? off : 0), size, on_done, NULL));
        xSemaphoreTake(s_done, portMAX_DELAY);
    }
    int64_t t1 = esp_timer_get_time();
    report(name, TOTAL, t1 - t0);
}

static void run_dma(size_t burst)
{
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    cfg.dma_burst_size = burst;
    async_memcpy_handle_t mcp = NULL;
    esp_err_t err = esp_async_memcpy_install_gdma_axi(&cfg, &mcp);
    if (err != ESP_OK) {
        printf("DMA burst %u: install failed (%s)\n", (unsigned)burst, esp_err_to_name(err));
        return;
    }
    printf("-- DMA (AXI GDMA), burst %u bytes --\n", (unsigned)burst);
    const size_t chunk = 1024 * 1024;  // 1 回の DMA で 1MB
    dma_copy(mcp, "PSRAM -> PSRAM (1MB)", s_ps_b, true, s_ps_a, true, chunk);
    dma_copy(mcp, "PSRAM -> internal (128KB)", s_int_a, false, s_ps_a, true, INT_BUF);
    dma_copy(mcp, "internal -> PSRAM (128KB)", s_ps_b, true, s_int_a, false, INT_BUF);
    dma_copy(mcp, "internal -> internal (128KB)", s_int_b, false, s_int_a, false, INT_BUF);
    ESP_ERROR_CHECK(esp_async_memcpy_uninstall(mcp));
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する
    printf("PSRAM %d MHz, CPU %d MHz, L2 cache %d KB, total %d MB per test\n", CONFIG_SPIRAM_SPEED,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_CACHE_L2_CACHE_SIZE / 1024, TOTAL / (1024 * 1024));

    s_ps_a = heap_caps_aligned_alloc(128, PSRAM_BUF, MALLOC_CAP_SPIRAM);
    s_ps_b = heap_caps_aligned_alloc(128, PSRAM_BUF, MALLOC_CAP_SPIRAM);
    s_int_a = heap_caps_aligned_alloc(128, INT_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_int_b = heap_caps_aligned_alloc(128, INT_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_done = xSemaphoreCreateBinary();
    if (!s_ps_a || !s_ps_b || !s_int_a || !s_int_b || !s_done) {
        printf("alloc failed\n");
        return;
    }
    printf("PSRAM buf %p %p, internal buf %p %p\n", s_ps_a, s_ps_b, s_int_a, s_int_b);
    memset(s_ps_a, 0x5a, PSRAM_BUF);
    memset(s_int_a, 0xa5, INT_BUF);

    printf("-- CPU --\n");
    cpu_read("read PSRAM 4MB", s_ps_a, PSRAM_BUF);
    cpu_read("read PSRAM 32KB (in L1)", s_ps_a, SMALL_BUF);
    cpu_read("read internal 128KB", s_int_a, INT_BUF);
    cpu_write("write PSRAM 4MB", s_ps_b, PSRAM_BUF, false);
    cpu_write("write PSRAM 4MB + writeback", s_ps_b, PSRAM_BUF, true);
    cpu_memcpy("memcpy PSRAM -> PSRAM 4MB", s_ps_b, true, s_ps_a, true, PSRAM_BUF);
    cpu_memcpy("memcpy PSRAM -> internal 128KB", s_int_a, false, s_ps_a, true, INT_BUF);
    cpu_memcpy("memcpy internal -> PSRAM 128KB", s_ps_b, true, s_int_a, false, INT_BUF);
    cpu_memcpy("memcpy internal -> internal 128KB", s_int_b, false, s_int_a, false, INT_BUF);

    run_dma(16);
    run_dma(32);
    run_dma(64);
    run_dma(128);

    // DMA で写した中身が合っているか（internal -> PSRAM で、PSRAM の 4MB 全体に内蔵 RAM の中身が並んでいる）
    esp_cache_msync(s_ps_b, INT_BUF, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    printf("check: %s\n", memcmp(s_ps_b, s_int_a, INT_BUF) == 0 ? "OK" : "NG");
    printf("done\n");
}
