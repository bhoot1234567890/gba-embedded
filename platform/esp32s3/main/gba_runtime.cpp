/*
 * GBA emulator runtime for ESP32-S3.
 * Dual-core pipeline: Core 1 runs emulator, Core 0 drives SPI display.
 */

#include <atomic>

#include "gba_display.h"
#include "gba/core/constants.hpp"
#include "gba/core/downscale.hpp"
#include "gba/core/emulator.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* kTag = "gba_runtime";

using namespace gba;

struct RuntimeContext {
    Emulator* emulator;
    DownscaleLut lut;
    u16* display_bufs[2];        /* ping-pong buffers in PSRAM */
    std::atomic<int> front{0};   /* index Core 0 is reading from */
    std::atomic<int> back{1};    /* index Core 1 is writing to */
    TaskHandle_t display_task;
};

static void cpu_task(void* arg) {
    auto* ctx = static_cast<RuntimeContext*>(arg);

    ESP_LOGI(kTag, "CPU task started on core %d", xPortGetCoreID());

    int frame_count = 0;
    while (true) {
        ctx->emulator->run_frame();

        /* Downscale 240x160 → 128x128 into back buffer */
        const auto fb = ctx->emulator->framebuffer();
        const int buf = ctx->back.load();
        downscale_565(ctx->lut, fb.data(), ctx->display_bufs[buf]);

        /* Swap: what we just wrote becomes the front buffer */
        ctx->front.store(buf);
        ctx->back.store(1 - buf);

        /* Signal Core 0 that a new frame is ready */
        xTaskNotifyGive(ctx->display_task);

        frame_count++;
        if (frame_count % 60 == 0) {
            ESP_LOGI(kTag, "Emulated %d frames", frame_count);
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "=== GBA Emulator Runtime (ESP32-S3) ===");

    /* Show heap info */
    ESP_LOGI(kTag, "Free internal: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(kTag, "Free PSRAM:    %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Initialize display */
    ESP_LOGI(kTag, "Initializing ST7735 display...");
    ESP_ERROR_CHECK(display_init());
    display_backlight(true);

    /* Fill display with test gradient */
    {
        auto* test_buf = new u16[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        for (int y = 0; y < DISPLAY_HEIGHT; y++) {
            for (int x = 0; x < DISPLAY_WIDTH; x++) {
                const auto r = static_cast<u16>((x * 31u) / 127u);
                const auto g = static_cast<u16>((y * 63u) / 127u);
                const auto b = u16{16};
                test_buf[y * DISPLAY_WIDTH + x] = static_cast<u16>((r << 11u) | (g << 5u) | b);
            }
        }
        display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, test_buf);
        delete[] test_buf;
        ESP_LOGI(kTag, "Test gradient sent to display");
    }

    /* Set up emulator */
    ESP_LOGI(kTag, "Allocating emulator...");
    auto* emulator = new Emulator();
    if (!emulator) {
        ESP_LOGE(kTag, "Failed to allocate Emulator");
        return;
    }
    emulator->reset();

    /* Set up runtime context */
    RuntimeContext ctx;
    ctx.emulator = emulator;
    ctx.lut = make_downscale_lut();
    ctx.display_bufs[0] = new u16[128 * 128];
    ctx.display_bufs[1] = new u16[128 * 128];
    ctx.front.store(0);
    ctx.back.store(1);
    ctx.display_task = xTaskGetCurrentTaskHandle();

    if (!ctx.display_bufs[0] || !ctx.display_bufs[1]) {
        ESP_LOGE(kTag, "Failed to allocate display buffers");
        return;
    }

    ESP_LOGI(kTag, "Free heap after alloc: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    /* Launch CPU task on Core 1 */
    xTaskCreatePinnedToCore(cpu_task, "gba_cpu", 32768, &ctx, 5, nullptr, 1);

    /* Core 0: display loop */
    ESP_LOGI(kTag, "Display loop running on core %d", xPortGetCoreID());
    while (true) {
        /* Wait for frame from Core 1 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int buf = ctx.front.load();
        display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, ctx.display_bufs[buf]);
    }
}
