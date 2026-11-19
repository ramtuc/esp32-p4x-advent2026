// ESP32-P4 の HP コアの CSR（命令セット・ID）を両方のコアで読み、Flash の定数を読む速さで L1/L2 キャッシュの効き方を見る
// （ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "riscv/csr.h"
#include "sdkconfig.h"

// Flash に置く定数（.rodata）。0 以外で埋めて、確実に Flash に置かれるようにする
#define FLASH_BUF_SIZE (384 * 1024)
static const uint32_t s_flash_buf[FLASH_BUF_SIZE / 4] = {[0 ... (FLASH_BUF_SIZE / 4 - 1)] = 0x5a5a5a5a};

#define LINE 64  // L1・L2 のキャッシュラインは 64 バイト（既定）

static void print_csr(void *arg)
{
    uint32_t misa = RV_READ_CSR(misa);
    char ext[27] = {0};
    int n = 0;
    for (int i = 0; i < 26; i++) {
        if (misa & (1u << i)) {
            ext[n++] = 'A' + i;
        }
    }
    printf("core %d: mhartid=%" PRIu32 " misa=0x%08" PRIx32 " (MXL=%" PRIu32 ", %s) mvendorid=0x%08" PRIx32
           " marchid=0x%08" PRIx32 " mimpid=0x%08" PRIx32 "\n",
           esp_cpu_get_core_id(), (uint32_t)RV_READ_CSR(mhartid), misa, misa >> 30, ext,
           (uint32_t)RV_READ_CSR(mvendorid), (uint32_t)RV_READ_CSR(marchid), (uint32_t)RV_READ_CSR(mimpid));
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

// buf の先頭 size バイトを、64 バイトおきに 1 語ずつ読む。passes 回まわした 1 回の読みの平均サイクルを返す
static float cycles_per_line(const volatile uint32_t *buf, size_t size, int passes)
{
    size_t lines = size / LINE;
    uint32_t sum = 0;
    for (size_t i = 0; i < lines; i++) {  // 1 回目は温める（測らない）
        sum += buf[i * (LINE / 4)];
    }
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int p = 0; p < passes; p++) {
        for (size_t i = 0; i < lines; i++) {
            sum += buf[i * (LINE / 4)];
        }
    }
    uint32_t t1 = esp_cpu_get_cycle_count();
    if (sum == 1) {
        printf(" ");  // 最適化で消されないように sum を使う
    }
    return (float)(t1 - t0) / ((float)lines * passes);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));  // USB の再接続を待ってから表示する

    printf("CPU %d MHz, L2 cache %d KB (line %d B), L1 line %d B\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           CONFIG_CACHE_L2_CACHE_SIZE / 1024, CONFIG_CACHE_L2_CACHE_LINE_SIZE, CONFIG_CACHE_L1_CACHE_LINE_SIZE);

    // CSR を両方のコアで読む
    for (int core = 0; core < 2; core++) {
        xTaskCreatePinnedToCore(print_csr, "csr", 4096, xTaskGetCurrentTaskHandle(), 5, NULL, core);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    // Flash の定数（キャッシュ経由）を、読む範囲を変えて読む
    printf("flash rodata @ %p\n", (void *)s_flash_buf);
    printf("%8s %14s %10s\n", "size", "cycles/line", "ns/line");
    const size_t sizes[] = {8, 16, 32, 48, 64, 96, 112, 128, 160, 192, 256, 384};
    for (int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i] * 1024;
        int passes = (int)((4 * 1024 * 1024) / size);  // どの大きさでも 4MB 分読む
        float c = cycles_per_line(s_flash_buf, size, passes);
        printf("%6u KB %14.1f %10.1f\n", (unsigned)sizes[i], c, c * 1000.0f / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }

    // 比べる相手: 内蔵 RAM（L2MEM）に取った同じ大きさの領域
    size_t ram_size = 256 * 1024;
    uint32_t *ram = heap_caps_malloc(ram_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ram) {
        for (size_t i = 0; i < ram_size / 4; i++) {
            ram[i] = i;
        }
        printf("internal RAM @ %p\n", (void *)ram);
        const size_t rsizes[] = {16, 32, 64, 128, 256};
        for (int i = 0; i < sizeof(rsizes) / sizeof(rsizes[0]); i++) {
            size_t size = rsizes[i] * 1024;
            int passes = (int)((4 * 1024 * 1024) / size);
            float c = cycles_per_line(ram, size, passes);
            printf("%6u KB %14.1f %10.1f\n", (unsigned)rsizes[i], c, c * 1000.0f / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        }
        free(ram);
    }
    printf("done\n");
}
