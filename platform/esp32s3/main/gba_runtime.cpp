/*
 * GBA emulator runtime for ESP32-S3.
 * Dual-core pipeline: Core 1 runs emulator, Core 0 drives SPI display.
 */

#include <atomic>
#include <span>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <new>
#include <dirent.h>
#include <strings.h>

#include "gba_audio.h"
#include "gba_board_profile.h"
#include "gba_display.h"
#include "gba_input.h"
#include "gba/core/constants.hpp"
#include "gba/core/emulator.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_pm.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "font5x7.h"

static const char* kTag = "gba_runtime";
static constexpr int kDisplayChunkRows = 16;
static constexpr int64_t kFrameBudgetUs = 16667;

using namespace gba;

struct RuntimeStats {
    uint64_t total_frame_us = 0;
    uint32_t total_frames = 0;
    uint32_t skipped_frames = 0;
    uint32_t audio_chunks = 0;
    uint32_t audio_errors = 0;
};

struct RuntimeContext {
    Emulator* emulator;
    u16* display_bufs[2];        /* ping-pong buffers in PSRAM */
    std::atomic<int> front{0};   /* index Core 0 is reading from */
    std::atomic<int> back{1};    /* index Core 1 is writing to */
    TaskHandle_t display_task;
    RuntimeStats stats{};
};

static esp_err_t mount_sdcard(sdmmc_card_t** out_card) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

#if GBA_STORAGE_MODE == GBA_STORAGE_MODE_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = GBA_SDMMC_PIN_CLK;
    slot_config.cmd = GBA_SDMMC_PIN_CMD;
    slot_config.d0 = GBA_SDMMC_PIN_D0;
    slot_config.d1 = GBA_SDMMC_PIN_D1;
    slot_config.d2 = GBA_SDMMC_PIN_D2;
    slot_config.d3 = GBA_SDMMC_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, out_card);
#elif GBA_STORAGE_MODE == GBA_STORAGE_MODE_SDSPI
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = GBA_SDSPI_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = static_cast<spi_host_device_t>(GBA_SDSPI_HOST);
    slot_config.gpio_miso = GBA_SDSPI_PIN_MISO;
    slot_config.gpio_mosi = GBA_SDSPI_PIN_MOSI;
    slot_config.gpio_sck = GBA_SDSPI_PIN_SCLK;
    slot_config.gpio_cs = GBA_SDSPI_PIN_CS;
    return esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, out_card);
#else
#error Unsupported GBA_STORAGE_MODE
#endif
}

static void draw_char(int x, int y, char c, u16 color, u16* buf) {
    if (c < 32 || c > 126) c = 32;
    const uint8_t* glyph = &font5x7[(c - 32) * 5];
    for (int col = 0; col < 5; ++col) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (line & 1) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                    buf[py * DISPLAY_WIDTH + px] = color;
                }
            }
            line >>= 1;
        }
    }
}

static void draw_string(int x, int y, const std::string& str, u16 color, u16* buf) {
    for (char c : str) {
        draw_char(x, y, c, color, buf);
        x += 6;
    }
}

static bool read_binary_file(const std::string& path, std::vector<u8>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);

    out.resize(static_cast<std::size_t>(size));
    return file.read(reinterpret_cast<char*>(out.data()), size).good();
}

static bool load_optional_bios(Emulator& emulator) {
    static constexpr const char* kBiosPaths[] = {
        "/sdcard/gba_bios.bin",
        "/sdcard/bios/gba_bios.bin",
        "/sdcard/BIOS/gba_bios.bin",
    };

    for (const char* path : kBiosPaths) {
        std::vector<u8> bios;
        if (!read_binary_file(path, bios)) {
            continue;
        }
        if (bios.size() != kBiosSize) {
            ESP_LOGW(kTag, "Ignoring BIOS %s: expected %u bytes, got %u",
                     path, kBiosSize, static_cast<unsigned>(bios.size()));
            continue;
        }
        emulator.load_bios(std::move(bios));
        ESP_LOGI(kTag, "Loaded BIOS from %s", path);
        return true;
    }

    ESP_LOGW(kTag, "No real BIOS found on SD; using skip-BIOS reset and integer BIOS HLE");
    return false;
}

static void convert_rgb555_to_rgb565_inplace(u16* pixels, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const u16 p = pixels[i];
        pixels[i] = static_cast<u16>(((p & 0x7C00u) << 1u) | ((p & 0x03E0u) << 1u) | (p & 0x001Fu));
    }
}

static void cpu_task(void* arg) {
    auto* ctx = static_cast<RuntimeContext*>(arg);

    ESP_LOGI(kTag, "CPU task started on core %d", xPortGetCoreID());
    if (esp_task_wdt_add(nullptr) != ESP_OK) {
        ESP_LOGW(kTag, "Failed to subscribe CPU task to watchdog");
    }

    int64_t window_start_us = esp_timer_get_time();
    bool skip_next_render = false;
    while (true) {
        const auto frame_start_us = esp_timer_get_time();
        (void)esp_task_wdt_reset();

        uint16_t keys = input_enabled() ? input_read_keys() : 0x03FF;
        const auto pressed_mask = static_cast<u16>((~keys) & 0x03FFu);
        ctx->emulator->set_keys(pressed_mask);

        const bool skip_render = skip_next_render;
        skip_next_render = false;
        ctx->emulator->set_skip_render(skip_render);
        const int buf = ctx->back.load();
        if (!skip_render) {
            ctx->emulator->ppu().set_external_fb(ctx->display_bufs[buf]);
            ctx->emulator->ppu().mark_all_dirty();
        }
        ctx->emulator->run_frame();

        if (!skip_render) {
            convert_rgb555_to_rgb565_inplace(ctx->display_bufs[buf], kOutputPixels);
        }

        if (audio_enabled() && ctx->emulator->apu().audio_chunk_ready()) {
            const auto chunk = ctx->emulator->apu().consume_audio_chunk();
            if (!chunk.empty()) {
                const auto ret = audio_write(chunk.data(), chunk.size());
                if (ret == ESP_OK) {
                    ++ctx->stats.audio_chunks;
                } else {
                    ++ctx->stats.audio_errors;
                }
            }
        }

        if (!skip_render) {
            ctx->front.store(buf);
            ctx->back.store(buf ^ 1);

            xTaskNotifyGive(ctx->display_task);
        } else {
            ++ctx->stats.skipped_frames;
        }

        const auto frame_end_us = esp_timer_get_time();
        const auto frame_us = frame_end_us - frame_start_us;
        ctx->stats.total_frame_us += static_cast<uint64_t>(frame_us);
        ++ctx->stats.total_frames;
        skip_next_render = frame_us > kFrameBudgetUs;

        if ((ctx->stats.total_frames % 60u) == 0u) {
            const auto window_us = frame_end_us - window_start_us;
            const auto avg_frame_us = static_cast<double>(ctx->stats.total_frame_us) /
                static_cast<double>(ctx->stats.total_frames);
            const auto fps = window_us > 0 ? (60.0 * 1000000.0) / static_cast<double>(window_us) : 0.0;
            ESP_LOGI(kTag, "Perf: avg_frame=%.2f ms fps=%.2f skipped=%u audio_chunks=%u audio_err=%u",
                     avg_frame_us / 1000.0, fps, ctx->stats.skipped_frames,
                     ctx->stats.audio_chunks, ctx->stats.audio_errors);
            window_start_us = frame_end_us;
            ctx->stats.total_frame_us = 0;
            ctx->stats.total_frames = 0;
            ctx->stats.skipped_frames = 0;
            ctx->stats.audio_chunks = 0;
            ctx->stats.audio_errors = 0;
        }
    }
}

extern "C" void app_main() {
    ESP_LOGI(kTag, "Initializing GBA runtime...");

#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config;
    pm_config.max_freq_mhz = 240;
    pm_config.min_freq_mhz = 240; // Lock to 240MHz Performance Profile
    pm_config.light_sleep_enable = false;
    esp_pm_configure(&pm_config);
#endif

    ESP_ERROR_CHECK(display_init());
    display_backlight(true);

    const auto input_ret = input_init();
    if (input_ret != ESP_OK && input_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(kTag, "Input init failed");
    }

    const auto audio_ret = audio_init();
    if (audio_ret != ESP_OK && audio_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(kTag, "Audio init failed: %s", esp_err_to_name(audio_ret));
    }

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = mount_sdcard(&card);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return;
    }

    std::vector<std::string> rom_files;
    DIR* dir = opendir("/sdcard");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcasecmp(ent->d_name + len - 4, ".gba") == 0) {
                rom_files.push_back(ent->d_name);
            }
        }
        closedir(dir);
    }
    
    if (rom_files.empty()) {
        ESP_LOGE(kTag, "No .gba files found on SD card");
        return;
    }

    std::sort(rom_files.begin(), rom_files.end());

    int selected_idx = 0;
    bool rom_selected = false;
    
    u16* ui_buf = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
    
    while (!rom_selected) {
        std::fill_n(ui_buf, 128 * 128, 0x0000);
        draw_string(4, 4, "ESP32 GBA EMULATOR", 0x07E0, ui_buf);
        draw_string(4, 14, "----------------", 0xFFFF, ui_buf);
        
        for (int i = 0; i < std::min(10, (int)rom_files.size()); ++i) {
            int file_idx = selected_idx - 4 + i;
            if (file_idx >= 0 && file_idx < (int)rom_files.size()) {
                u16 color = (file_idx == selected_idx) ? 0x07E0 : 0x7BEF;
                draw_string(10, 26 + i * 10, rom_files[file_idx].substr(0, 18), color, ui_buf);
                if (file_idx == selected_idx) {
                    draw_string(2, 26 + i * 10, ">", 0x07E0, ui_buf);
                }
            }
        }
        
        display_draw(0, 0, 128, 128, ui_buf);
        
        uint16_t keys = input_enabled() ? input_read_keys() : 0x03FF;
        if ((keys & gba::kKeyA) == 0) {
            rom_selected = true;
        }
        if ((keys & gba::kKeyUp) == 0) {
            if (selected_idx > 0) selected_idx--;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if ((keys & gba::kKeyDown) == 0) {
            if (selected_idx < (int)rom_files.size() - 1) selected_idx++;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    
    std::string selected_rom_path = "/sdcard/" + rom_files[selected_idx];
    std::string selected_sav_path = selected_rom_path.substr(0, selected_rom_path.length() - 4) + ".sav";
    
    // UI Loading state
    std::fill_n(ui_buf, 128 * 128, 0x0000);
    draw_string(4, 50, "LOADING...", 0x07E0, ui_buf);
    draw_string(4, 60, rom_files[selected_idx].substr(0, 18), 0xFFFF, ui_buf);
    display_draw(0, 0, 128, 128, ui_buf);
    heap_caps_free(ui_buf);

    auto* emulator = new (std::nothrow) Emulator{};
    if (!emulator) {
        ESP_LOGE(kTag, "Failed to allocate Emulator");
        return;
    }
    const bool bios_loaded = load_optional_bios(*emulator);

    std::vector<u8> rom_data;
    if (!read_binary_file(selected_rom_path, rom_data)) {
        ESP_LOGE(kTag, "Failed to read ROM %s", selected_rom_path.c_str());
        return;
    }

    emulator->load_rom(std::move(rom_data));
    emulator->cartridge().auto_detect_save_type();

    std::vector<u8> save_data;
    if (read_binary_file(selected_sav_path, save_data)) {
        emulator->cartridge().load_save(std::move(save_data));
    }

    emulator->ppu().set_direct_128x128(true);
    emulator->reset(!bios_loaded);

    RuntimeContext ctx;
    ctx.emulator = emulator;
    ctx.display_bufs[0] = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
    ctx.display_bufs[1] = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
    ctx.front.store(0);
    ctx.back.store(1);
    ctx.display_task = xTaskGetCurrentTaskHandle();

    xTaskCreatePinnedToCore(cpu_task, "gba_cpu", 32768, &ctx, 5, nullptr, 1);

    int64_t last_save_dirty_time = 0;
    bool save_pending = false;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int buf = ctx.front.load();
        for (int y = 0; y < DISPLAY_HEIGHT; y += kDisplayChunkRows) {
            const auto rows = std::min(kDisplayChunkRows, DISPLAY_HEIGHT - y);
            const auto* pixels = ctx.display_bufs[buf] + (y * DISPLAY_WIDTH);
            const auto draw_ret = display_draw(0, y, DISPLAY_WIDTH, rows, pixels);
            if (draw_ret != ESP_OK) {
                ESP_LOGW(kTag, "display_draw failed at y=%d: %s", y, esp_err_to_name(draw_ret));
                break;
            }
        }

        // Debounced Save Data Write-Back
        if (ctx.emulator->cartridge().is_save_dirty()) {
            ctx.emulator->cartridge().clear_save_dirty();
            last_save_dirty_time = esp_timer_get_time();
            save_pending = true;
        } else if (save_pending && (esp_timer_get_time() - last_save_dirty_time > 2000000)) { // 2 seconds
            auto save_data = ctx.emulator->cartridge().save();
            if (!save_data.empty()) {
                std::ofstream out_save(selected_sav_path, std::ios::binary | std::ios::trunc);
                if (out_save) {
                    out_save.write(reinterpret_cast<const char*>(save_data.data()), save_data.size());
                    out_save.close();
                }
            }
            save_pending = false;
        }
    }
}
