/*
 * Minimal display-only firmware for validating ESP32-S3 wiring and panel config.
 */

#include "gba_display.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* kTag = "display_test";

static constexpr uint16_t kBlack = 0x0000;
static constexpr uint16_t kWhite = 0xFFFF;
static constexpr uint16_t kRed = 0xF800;
static constexpr uint16_t kGreen = 0x07E0;
static constexpr uint16_t kBlue = 0x001F;
static constexpr uint16_t kYellow = 0xFFE0;

static void fill_rect(uint16_t* buf, int x, int y, int w, int h, uint16_t color) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(DISPLAY_WIDTH, x + w);
    const int y1 = std::min(DISPLAY_HEIGHT, y + h);
    for (int row = y0; row < y1; ++row) {
        std::fill(buf + row * DISPLAY_WIDTH + x0, buf + row * DISPLAY_WIDTH + x1, color);
    }
}

static void draw_border(uint16_t* buf, uint16_t color) {
    fill_rect(buf, 0, 0, DISPLAY_WIDTH, 1, color);
    fill_rect(buf, 0, DISPLAY_HEIGHT - 1, DISPLAY_WIDTH, 1, color);
    fill_rect(buf, 0, 0, 1, DISPLAY_HEIGHT, color);
    fill_rect(buf, DISPLAY_WIDTH - 1, 0, 1, DISPLAY_HEIGHT, color);
}

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "SSD/ST display test start: %dx%d, pins sclk=%d mosi=%d cs=%d dc=%d rst=%d bl=%d",
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             static_cast<int>(DISPLAY_PIN_SCLK), static_cast<int>(DISPLAY_PIN_MOSI),
             static_cast<int>(DISPLAY_PIN_CS), static_cast<int>(DISPLAY_PIN_DC),
             static_cast<int>(DISPLAY_PIN_RST), static_cast<int>(DISPLAY_PIN_BL));

    ESP_ERROR_CHECK(display_init());

    const size_t pixels = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
    auto* buf = static_cast<uint16_t*>(
        heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!buf) {
        ESP_LOGW(kTag, "internal DMA frame buffer failed, trying DMA-capable heap");
        buf = static_cast<uint16_t*>(heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA));
    }
    if (!buf) {
        ESP_LOGE(kTag, "failed to allocate %u-byte test frame buffer",
                 static_cast<unsigned>(pixels * sizeof(uint16_t)));
        abort();
    }

    uint32_t frame = 0;
    while (true) {
        std::fill_n(buf, pixels, kBlack);
        fill_rect(buf, 2, 2, DISPLAY_WIDTH - 4, DISPLAY_HEIGHT / 3 - 2, kRed);
        fill_rect(buf, 2, DISPLAY_HEIGHT / 3, DISPLAY_WIDTH - 4, DISPLAY_HEIGHT / 3 - 1, kGreen);
        fill_rect(buf, 2, (DISPLAY_HEIGHT * 2) / 3, DISPLAY_WIDTH - 4,
                  DISPLAY_HEIGHT - (DISPLAY_HEIGHT * 2) / 3 - 2, kBlue);
        draw_border(buf, kWhite);

        const int travel = std::max(1, DISPLAY_WIDTH - 20);
        const int x = 2 + static_cast<int>(frame % travel);
        const int y = DISPLAY_HEIGHT / 2 - 8;
        fill_rect(buf, x, y, 16, 16, kYellow);

        esp_err_t ret = display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, buf);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "display_draw failed: %s", esp_err_to_name(ret));
        }

        if ((frame % 60) == 0) {
            ESP_LOGI(kTag, "display frames=%lu free_heap=%lu",
                     static_cast<unsigned long>(frame),
                     static_cast<unsigned long>(esp_get_free_heap_size()));
        }

        ++frame;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
