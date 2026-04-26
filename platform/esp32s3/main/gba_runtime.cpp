/*
 * GBA emulator runtime for ESP32-S3.
 * Dual-core pipeline: Core 1 runs emulator, Core 0 drives SPI display.
 */

#include <atomic>
#include <span>

#include "gba_display.h"
#include "gba/core/constants.hpp"
#include "gba/core/downscale.hpp"
#include "gba/core/emulator.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <fstream>
#include <vector>

/* SD Card SDIO Pins (Default for standard ESP32-S3 generic breakout, 4-bit) */
#ifndef SD_PIN_CLK
#define SD_PIN_CLK  GPIO_NUM_14
#define SD_PIN_CMD  GPIO_NUM_15
#define SD_PIN_D0   GPIO_NUM_2
#define SD_PIN_D1   GPIO_NUM_4
#define SD_PIN_D2   GPIO_NUM_12
#define SD_PIN_D3   GPIO_NUM_13
#endif

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
        esp_task_wdt_reset();
        ctx->emulator->set_keys(0x03FF); // all buttons released
        ctx->emulator->run_frame();

        /* Downscale 240x160 → 128x128 into back buffer */
        const auto fb = ctx->emulator->framebuffer();
        const int buf = ctx->back.load();
        downscale_565_v2(ctx->lut, fb.data(), ctx->display_bufs[buf]);

        /* Swap buffers */
        ctx->front.store(buf);
        ctx->back.store(buf ^ 1);

        /* Notify Core 0 to draw */
        xTaskNotifyGive(ctx->display_task);

        if (++frame_count % 60 == 0) {
            ESP_LOGI(kTag, "Emulated 60 frames");
        }
    }
}

extern "C" void app_main() {
    ESP_LOGI(kTag, "Initializing GBA runtime...");

    /* Init SPI display */
    ESP_ERROR_CHECK(display_init());
    display_backlight(true);

    /* Allocate emulator in PSRAM */
    auto* emulator = new (std::nothrow) Emulator{};
    if (!emulator) {
        ESP_LOGE(kTag, "Failed to allocate Emulator");
        return;
    }
    emulator->reset();

    /* Initialize SD Card and mount FATFS */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = SD_PIN_D1;
    slot_config.d2 = SD_PIN_D2;
    slot_config.d3 = SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t* card;
    ESP_LOGI(kTag, "Mounting SD card...");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount SD card (%s)", esp_err_to_name(ret));
        return;
    }
    sdmmc_card_print_info(stdout, card);

    /* Load ROM from SD Card */
    ESP_LOGI(kTag, "Loading /sdcard/rom.gba ...");
    std::ifstream file("/sdcard/rom.gba", std::ios::binary | std::ios::ate);
    if (!file) {
        ESP_LOGE(kTag, "Failed to open /sdcard/rom.gba");
        return;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    ESP_LOGI(kTag, "ROM size: %d bytes", static_cast<int>(size));
    std::vector<u8> rom_data(size);
    if (!file.read(reinterpret_cast<char*>(rom_data.data()), size)) {
        ESP_LOGE(kTag, "Failed to read ROM data");
        return;
    }
    file.close();

    emulator->load_rom(std::move(rom_data));

    /* Set up runtime context */
    RuntimeContext ctx;
    ctx.emulator = emulator;
    ctx.lut = make_downscale_lut();
    ctx.display_bufs[0] = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
    ctx.display_bufs[1] = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
    ctx.front.store(0);
    ctx.back.store(1);
    ctx.display_task = xTaskGetCurrentTaskHandle();

    if (!ctx.display_bufs[0] || !ctx.display_bufs[1]) {
        ESP_LOGE(kTag, "Failed to allocate display buffers");
        return;
    }

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
