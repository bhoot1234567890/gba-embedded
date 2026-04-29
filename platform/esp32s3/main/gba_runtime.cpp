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
#include "gba/platform/esp32_rom_provider.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_pm.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "font5x7.h"

static const char* kTag = "gba_runtime";
static constexpr int kDisplayChunkRows = 16;
static constexpr int64_t kFrameBudgetUs = 16667;
static_assert(DISPLAY_WIDTH <= gba::kOutW && DISPLAY_HEIGHT <= gba::kOutH,
              "ESP32 display must fit inside the direct 128x128 PPU output");

using namespace gba;

static bool valid_pin(gpio_num_t pin) {
    return static_cast<int>(pin) >= 0;
}

static bool same_valid_pin(gpio_num_t lhs, gpio_num_t rhs) {
    return valid_pin(lhs) && valid_pin(rhs) && lhs == rhs;
}

static bool conflicts_with_display_pin(gpio_num_t pin) {
    return same_valid_pin(pin, GBA_DISPLAY_PIN_SCLK) ||
           same_valid_pin(pin, GBA_DISPLAY_PIN_MOSI) ||
           same_valid_pin(pin, GBA_DISPLAY_PIN_CS) ||
           same_valid_pin(pin, GBA_DISPLAY_PIN_DC) ||
           same_valid_pin(pin, GBA_DISPLAY_PIN_RST) ||
           same_valid_pin(pin, GBA_DISPLAY_PIN_BL);
}

struct RuntimeStats {
    uint64_t total_frame_us = 0;
    uint32_t total_frames = 0;
    uint32_t skipped_frames = 0;
    uint32_t audio_chunks = 0;
    uint32_t audio_errors = 0;
    uint64_t rom_cache_misses = 0;
    uint64_t rom_prefetch_hits = 0;
    uint64_t rom_prefetch_misses = 0;
    uint64_t rom_miss_penalty_us = 0;
};

struct RuntimeContext {
    Emulator* emulator;
    u16* display_bufs[2];        /* ping-pong buffers in PSRAM */
    std::atomic<int> front{0};   /* index Core 0 is reading from */
    std::atomic<int> back{1};    /* index Core 1 is writing to */
    TaskHandle_t display_task;
    RuntimeStats stats{};
};

[[maybe_unused]] static esp_err_t mount_sdcard(sdmmc_card_t** out_card) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

#if GBA_STORAGE_MODE == GBA_STORAGE_MODE_SDMMC
    if (!valid_pin(GBA_SDMMC_PIN_CLK) || !valid_pin(GBA_SDMMC_PIN_CMD) ||
        !valid_pin(GBA_SDMMC_PIN_D0) || !valid_pin(GBA_SDMMC_PIN_D1) ||
        !valid_pin(GBA_SDMMC_PIN_D2) || !valid_pin(GBA_SDMMC_PIN_D3)) {
        ESP_LOGE(kTag, "SDMMC pins are not configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (conflicts_with_display_pin(GBA_SDMMC_PIN_CLK) ||
        conflicts_with_display_pin(GBA_SDMMC_PIN_CMD) ||
        conflicts_with_display_pin(GBA_SDMMC_PIN_D0) ||
        conflicts_with_display_pin(GBA_SDMMC_PIN_D1) ||
        conflicts_with_display_pin(GBA_SDMMC_PIN_D2) ||
        conflicts_with_display_pin(GBA_SDMMC_PIN_D3)) {
        ESP_LOGE(kTag, "SDMMC pin map conflicts with display pins");
        return ESP_ERR_INVALID_STATE;
    }

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
    if (!valid_pin(GBA_SDSPI_PIN_MISO) || !valid_pin(GBA_SDSPI_PIN_MOSI) ||
        !valid_pin(GBA_SDSPI_PIN_SCLK) || !valid_pin(GBA_SDSPI_PIN_CS)) {
        ESP_LOGE(kTag, "SDSPI pins are not configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (conflicts_with_display_pin(GBA_SDSPI_PIN_MISO) ||
        conflicts_with_display_pin(GBA_SDSPI_PIN_MOSI) ||
        conflicts_with_display_pin(GBA_SDSPI_PIN_SCLK) ||
        conflicts_with_display_pin(GBA_SDSPI_PIN_CS)) {
        ESP_LOGE(kTag, "SDSPI pin map conflicts with display pins");
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = GBA_SDSPI_PIN_MOSI;
    bus_config.miso_io_num = GBA_SDSPI_PIN_MISO;
    bus_config.sclk_io_num = GBA_SDSPI_PIN_SCLK;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 16 * 1024;

    esp_err_t spi_ret = spi_bus_initialize(static_cast<spi_host_device_t>(GBA_SDSPI_HOST),
                                           &bus_config, SDSPI_DEFAULT_DMA);
    if (spi_ret != ESP_OK && spi_ret != ESP_ERR_INVALID_STATE) {
        return spi_ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = GBA_SDSPI_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = static_cast<spi_host_device_t>(GBA_SDSPI_HOST);
    slot_config.gpio_cs = GBA_SDSPI_PIN_CS;
    slot_config.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_config.gpio_wp = SDSPI_SLOT_NO_WP;
    slot_config.gpio_int = SDSPI_SLOT_NO_INT;
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

static void fill_rect(u16* buf, int x, int y, int w, int h, u16 color) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(DISPLAY_WIDTH, x + w);
    const int y1 = std::min(DISPLAY_HEIGHT, y + h);
    for (int row = y0; row < y1; ++row) {
        std::fill(buf + row * DISPLAY_WIDTH + x0, buf + row * DISPLAY_WIDTH + x1, color);
    }
}

static void draw_message(u16* buf,
                         size_t display_pixels,
                         const std::string& title,
                         const std::string& detail) {
    std::fill_n(buf, display_pixels, 0x0000);
    fill_rect(buf, 0, 0, DISPLAY_WIDTH, 14, 0xF800);
    fill_rect(buf, 0, DISPLAY_HEIGHT - 14, DISPLAY_WIDTH, 14, 0x001F);
    draw_string(4, 4, "GBA ESP32", 0xFFFF, buf);
    draw_string(4, 24, title.substr(0, 20), 0xFFE0, buf);
    draw_string(4, 38, detail.substr(0, 20), 0xFFFF, buf);
    draw_string(4, 60, "CHECK PIN CONFIG", 0x07E0, buf);
}

[[noreturn]] static void halt_with_message(u16* buf,
                                           size_t display_pixels,
                                           const std::string& title,
                                           const std::string& detail) {
    bool alternate = false;
    while (true) {
        draw_message(buf, display_pixels, title, detail);
        if (alternate) {
            fill_rect(buf, DISPLAY_WIDTH - 18, 18, 12, 12, 0xFFE0);
        } else {
            fill_rect(buf, DISPLAY_WIDTH - 18, 18, 12, 12, 0x07E0);
        }
        const auto ret = display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, buf);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "display_draw message failed: %s", esp_err_to_name(ret));
        }
        alternate = !alternate;
        vTaskDelay(pdMS_TO_TICKS(500));
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

[[maybe_unused]] static bool load_optional_bios(Emulator& emulator) {
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

static std::unique_ptr<RomProvider> open_rom_provider(const std::string& sd_rom_path) {
#if GBA_ROM_BACKEND == GBA_ROM_BACKEND_FLASH_MMAP || GBA_ROM_BACKEND == GBA_ROM_BACKEND_AUTO
    auto mmap_provider = make_esp32_mmap_rom_provider(
        GBA_ROM_MMAP_PARTITION_LABEL,
        kMaxRomSize,
        GBA_ROM_MMAP_WINDOW_BYTES,
        GBA_ROM_MMAP_WINDOW_COUNT);
    if (mmap_provider) {
        ESP_LOGI(kTag, "ROM source: FLASH_MMAP");
        return mmap_provider;
    }
#endif

#if GBA_ROM_BACKEND == GBA_ROM_BACKEND_SD_CACHE || GBA_ROM_BACKEND == GBA_ROM_BACKEND_AUTO
    auto sd_provider = make_esp32_sd_cache_rom_provider(
        sd_rom_path.c_str(),
        GBA_ROM_SD_CACHE_BYTES,
        GBA_ROM_SD_CACHE_PAGE_BYTES);
    if (sd_provider) {
        ESP_LOGI(kTag, "ROM source: SD_CACHE");
        return sd_provider;
    }
#endif

    return nullptr;
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
        const auto rom_stats = ctx->emulator->last_rom_stats();
        ctx->stats.rom_cache_misses += rom_stats.cache_misses;
        ctx->stats.rom_prefetch_hits += rom_stats.prefetch_hits;
        ctx->stats.rom_prefetch_misses += rom_stats.prefetch_misses;
        ctx->stats.rom_miss_penalty_us += rom_stats.miss_penalty_us;

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
            ESP_LOGI(kTag, "Perf: avg_frame=%.2f ms fps=%.2f skipped=%u audio_chunks=%u audio_err=%u rom_miss=%llu rom_pfh=%llu rom_pfm=%llu rom_miss_ms=%.2f",
                     avg_frame_us / 1000.0, fps, ctx->stats.skipped_frames,
                     ctx->stats.audio_chunks, ctx->stats.audio_errors,
                     static_cast<unsigned long long>(ctx->stats.rom_cache_misses),
                     static_cast<unsigned long long>(ctx->stats.rom_prefetch_hits),
                     static_cast<unsigned long long>(ctx->stats.rom_prefetch_misses),
                     static_cast<double>(ctx->stats.rom_miss_penalty_us) / 1000.0);
            window_start_us = frame_end_us;
            ctx->stats.total_frame_us = 0;
            ctx->stats.total_frames = 0;
            ctx->stats.skipped_frames = 0;
            ctx->stats.audio_chunks = 0;
            ctx->stats.audio_errors = 0;
            ctx->stats.rom_cache_misses = 0;
            ctx->stats.rom_prefetch_hits = 0;
            ctx->stats.rom_prefetch_misses = 0;
            ctx->stats.rom_miss_penalty_us = 0;
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

    const size_t display_pixels = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
    u16* ui_buf = static_cast<u16*>(heap_caps_malloc(display_pixels * sizeof(u16), MALLOC_CAP_SPIRAM));
    if (!ui_buf) {
        ESP_LOGE(kTag, "Failed to allocate UI buffer");
        return;
    }

    std::string selected_rom_path;
    std::string selected_sav_path;
    std::string selected_display_name;

#if GBA_ROM_BACKEND == GBA_ROM_BACKEND_FLASH_MMAP
    selected_rom_path = GBA_ROM_MMAP_PARTITION_LABEL;
    selected_display_name = std::string("flash:") + GBA_ROM_MMAP_PARTITION_LABEL;
#else
    sdmmc_card_t* card = nullptr;
    esp_err_t ret = mount_sdcard(&card);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount SD card: %s", esp_err_to_name(ret));
        halt_with_message(ui_buf, display_pixels, "SD MOUNT FAIL", esp_err_to_name(ret));
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
        halt_with_message(ui_buf, display_pixels, "NO .GBA FILES", "COPY ROM TO SD");
    }

    std::sort(rom_files.begin(), rom_files.end());

    int selected_idx = 0;
    bool rom_selected = false;
    while (!rom_selected) {
        std::fill_n(ui_buf, display_pixels, 0x0000);
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
        
        display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui_buf);
        
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

    selected_rom_path = "/sdcard/" + rom_files[selected_idx];
    selected_sav_path = selected_rom_path.substr(0, selected_rom_path.length() - 4) + ".sav";
    selected_display_name = rom_files[selected_idx];
#endif

    std::fill_n(ui_buf, display_pixels, 0x0000);
    draw_string(4, 50, "LOADING...", 0x07E0, ui_buf);
    draw_string(4, 60, selected_display_name.substr(0, 18), 0xFFFF, ui_buf);
    display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui_buf);
    heap_caps_free(ui_buf);

    auto* emulator = new (std::nothrow) Emulator{};
    if (!emulator) {
        ESP_LOGE(kTag, "Failed to allocate Emulator");
        return;
    }
#if GBA_ROM_BACKEND == GBA_ROM_BACKEND_FLASH_MMAP
    const bool bios_loaded = false;
    ESP_LOGW(kTag, "Flash ROM backend selected; using skip-BIOS reset and integer BIOS HLE");
#else
    const bool bios_loaded = load_optional_bios(*emulator);
#endif

    u16* display_bufs[2] = {
        static_cast<u16*>(heap_caps_malloc(kOutputPixels * sizeof(u16), MALLOC_CAP_SPIRAM)),
        static_cast<u16*>(heap_caps_malloc(kOutputPixels * sizeof(u16), MALLOC_CAP_SPIRAM)),
    };
    if (!display_bufs[0] || !display_bufs[1]) {
        ESP_LOGE(kTag, "Failed to allocate display buffers");
        return;
    }

    auto rom_provider = open_rom_provider(selected_rom_path);
    if (!rom_provider) {
        ESP_LOGE(kTag, "Failed to open ROM provider for %s", selected_rom_path.c_str());
        return;
    }

    emulator->set_rom_provider(std::move(rom_provider));
    emulator->cartridge().auto_detect_save_type();

    std::vector<u8> save_data;
    if (!selected_sav_path.empty() && read_binary_file(selected_sav_path, save_data)) {
        emulator->cartridge().load_save(std::move(save_data));
    }

    emulator->ppu().set_direct_128x128(true);
    emulator->reset(!bios_loaded);

    RuntimeContext ctx;
    ctx.emulator = emulator;
    ctx.display_bufs[0] = display_bufs[0];
    ctx.display_bufs[1] = display_bufs[1];
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
            const auto* pixels = ctx.display_bufs[buf] + (y * kOutW);
            const auto draw_ret = display_draw(0, y, DISPLAY_WIDTH, rows, pixels);
            if (draw_ret != ESP_OK) {
                ESP_LOGW(kTag, "display_draw failed at y=%d: %s", y, esp_err_to_name(draw_ret));
                break;
            }
        }

        // Debounced Save Data Write-Back
        if (ctx.emulator->cartridge().is_save_dirty()) {
            ctx.emulator->cartridge().clear_save_dirty();
            if (!selected_sav_path.empty()) {
                last_save_dirty_time = esp_timer_get_time();
                save_pending = true;
            }
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
