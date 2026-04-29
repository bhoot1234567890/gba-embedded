/*
 * SPI display driver for ESP32-S3.
 * Supports ST7735 and SSD1351 using esp_lcd_panel_io_spi for DMA transfers.
 */

#include "gba_display.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* kTag = "display";
static esp_lcd_panel_io_handle_t s_io = nullptr;
static uint16_t* s_swap_buf = nullptr;
static size_t s_swap_pixels = 0;
static SemaphoreHandle_t s_color_done = nullptr;

static bool valid_gpio(gpio_num_t pin) {
    return static_cast<int>(pin) >= 0;
}

static uint64_t gpio_mask(gpio_num_t pin) {
    const int gpio = static_cast<int>(pin);
    return gpio >= 0 ? (1ULL << static_cast<unsigned>(gpio)) : 0;
}

static void cmd(uint8_t command, const uint8_t* params = nullptr, int param_len = 0) {
    esp_lcd_panel_io_tx_param(s_io, command, params, param_len);
}

static bool IRAM_ATTR color_trans_done_cb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user_ctx) {
    auto sem = static_cast<SemaphoreHandle_t>(user_ctx);
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static void hardware_reset() {
    if (!valid_gpio(DISPLAY_PIN_RST)) {
        vTaskDelay(pdMS_TO_TICKS(120));
        return;
    }

    gpio_set_level(DISPLAY_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t tx_pixels(uint8_t ram_cmd, const void* rgb565_data, size_t pixels) {
    if (!s_color_done) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_color_done, 0);

#if GBA_DISPLAY_SWAP_BYTES
    if (!s_swap_buf || s_swap_pixels < pixels) {
        return ESP_ERR_NO_MEM;
    }

    const auto* src = static_cast<const uint16_t*>(rgb565_data);
    for (size_t i = 0; i < pixels; ++i) {
        const uint16_t p = src[i];
        s_swap_buf[i] = static_cast<uint16_t>((p << 8) | (p >> 8));
    }
    const size_t bytes = pixels * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(esp_cache_msync(s_swap_buf, bytes,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                        kTag, "display swap cache sync failed");
    const esp_err_t ret = esp_lcd_panel_io_tx_color(s_io, ram_cmd, s_swap_buf, bytes);
#else
    const size_t bytes = pixels * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(esp_cache_msync(const_cast<void*>(rgb565_data), bytes,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                        kTag, "display source cache sync failed");
    const esp_err_t ret = esp_lcd_panel_io_tx_color(s_io, ram_cmd, rgb565_data, bytes);
#endif
    if (ret != ESP_OK) {
        return ret;
    }
    xSemaphoreTake(s_color_done, portMAX_DELAY);
    return ESP_OK;
}

#if GBA_DISPLAY_DRIVER == GBA_DISPLAY_DRIVER_ST7735

enum St7735Cmd : uint8_t {
    kSwReset  = 0x01,
    kSlpOut   = 0x11,
    kDispOn   = 0x29,
    kCaseT    = 0x2A,
    kRasT     = 0x2B,
    kRamWr    = 0x2C,
    kMadCtl   = 0x36,
    kColMod   = 0x3A,
    kInvOn    = 0x21,
    kNorOn    = 0x13,
    kFrmCtr1  = 0xB1,
    kFrmCtr2  = 0xB2,
    kFrmCtr3  = 0xB3,
    kInvCtr   = 0xB4,
    kPwCtr1   = 0xC0,
    kPwCtr2   = 0xC1,
    kPwCtr3   = 0xC2,
    kPwCtr4   = 0xC3,
    kPwCtr5   = 0xC4,
    kVmCtr1   = 0xC5,
};

static void panel_init() {
    hardware_reset();

    cmd(kSwReset);
    vTaskDelay(pdMS_TO_TICKS(120));

    cmd(kSlpOut);
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t frm1[] = {0x01, 0x2C, 0x2D};
    cmd(kFrmCtr1, frm1, 3);

    const uint8_t frm2[] = {0x01, 0x2C, 0x2D};
    cmd(kFrmCtr2, frm2, 3);

    const uint8_t frm3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    cmd(kFrmCtr3, frm3, 6);

    const uint8_t invctr[] = {0x07};
    cmd(kInvCtr, invctr, 1);

    const uint8_t pw1[] = {0xA2, 0x02, 0x84};
    cmd(kPwCtr1, pw1, 3);

    const uint8_t pw2[] = {0xC5};
    cmd(kPwCtr2, pw2, 1);

    const uint8_t pw3[] = {0x0A, 0x00};
    cmd(kPwCtr3, pw3, 2);

    const uint8_t pw4[] = {0x8A, 0x2A};
    cmd(kPwCtr4, pw4, 2);

    const uint8_t pw5[] = {0x8A, 0xEE};
    cmd(kPwCtr5, pw5, 2);

    const uint8_t vm1[] = {0x0E};
    cmd(kVmCtr1, vm1, 1);

    const uint8_t colmod[] = {0x05};
    cmd(kColMod, colmod, 1);

    const uint8_t madctl[] = {0x00};
    cmd(kMadCtl, madctl, 1);

    const uint8_t caset[] = {
        0x00, static_cast<uint8_t>(DISPLAY_COL_OFFSET),
        0x00, static_cast<uint8_t>(DISPLAY_COL_OFFSET + DISPLAY_WIDTH - 1)
    };
    cmd(kCaseT, caset, 4);

    const uint8_t raset[] = {
        0x00, static_cast<uint8_t>(DISPLAY_ROW_OFFSET),
        0x00, static_cast<uint8_t>(DISPLAY_ROW_OFFSET + DISPLAY_HEIGHT - 1)
    };
    cmd(kRasT, raset, 4);

    cmd(kInvOn);
    cmd(kNorOn);
    vTaskDelay(pdMS_TO_TICKS(10));

    cmd(kDispOn);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(kTag, "ST7735 initialized (%dx%d, offset %d,%d)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_COL_OFFSET, DISPLAY_ROW_OFFSET);
}

static esp_err_t set_window_and_write(int x, int y, int w, int h, const void* rgb565_data) {
    const uint8_t xs = static_cast<uint8_t>(DISPLAY_COL_OFFSET + x);
    const uint8_t xe = static_cast<uint8_t>(DISPLAY_COL_OFFSET + x + w - 1);
    const uint8_t ys = static_cast<uint8_t>(DISPLAY_ROW_OFFSET + y);
    const uint8_t ye = static_cast<uint8_t>(DISPLAY_ROW_OFFSET + y + h - 1);

    const uint8_t caset[] = {0x00, xs, 0x00, xe};
    cmd(kCaseT, caset, 4);
    const uint8_t raset[] = {0x00, ys, 0x00, ye};
    cmd(kRasT, raset, 4);

    return tx_pixels(kRamWr, rgb565_data, static_cast<size_t>(w) * h);
}

#elif GBA_DISPLAY_DRIVER == GBA_DISPLAY_DRIVER_SSD1351

enum Ssd1351Cmd : uint8_t {
    kSetColumn      = 0x15,
    kSetRow         = 0x75,
    kWriteRam       = 0x5C,
    kSetRemap       = 0xA0,
    kStartLine      = 0xA1,
    kDisplayOffset  = 0xA2,
    kNormalDisplay  = 0xA6,
    kDisplayOff     = 0xAE,
    kDisplayOn      = 0xAF,
    kFunctionSelect = 0xAB,
    kPrecharge      = 0xB1,
    kClockDiv       = 0xB3,
    kSetVsl         = 0xB4,
    kSetGpio        = 0xB5,
    kPrecharge2     = 0xB6,
    kSetCommandLock = 0xFD,
    kSetMuxRatio    = 0xCA,
    kContrastAbc    = 0xC1,
    kContrastMaster = 0xC7,
    kVcomh          = 0xBE,
};

static void panel_init() {
    hardware_reset();

    const uint8_t lock1[] = {0x12};
    cmd(kSetCommandLock, lock1, 1);
    const uint8_t lock2[] = {0xB1};
    cmd(kSetCommandLock, lock2, 1);

    cmd(kDisplayOff);

    const uint8_t clockdiv[] = {0xF1};
    cmd(kClockDiv, clockdiv, 1);

    const uint8_t mux[] = {static_cast<uint8_t>(GBA_DISPLAY_SSD1351_MUX_RATIO)};
    cmd(kSetMuxRatio, mux, 1);

    const uint8_t offset[] = {0x00};
    cmd(kDisplayOffset, offset, 1);

    const uint8_t gpio[] = {0x00};
    cmd(kSetGpio, gpio, 1);

    const uint8_t fnsel[] = {0x01};
    cmd(kFunctionSelect, fnsel, 1);

    const uint8_t precharge[] = {0x32};
    cmd(kPrecharge, precharge, 1);

    const uint8_t vcomh[] = {0x05};
    cmd(kVcomh, vcomh, 1);

    cmd(kNormalDisplay);

    const uint8_t contrast[] = {0xC8, 0x80, 0xC8};
    cmd(kContrastAbc, contrast, 3);

    const uint8_t master[] = {0x0F};
    cmd(kContrastMaster, master, 1);

    const uint8_t vsl[] = {0xA0, 0xB5, 0x55};
    cmd(kSetVsl, vsl, 3);

    const uint8_t precharge2[] = {0x01};
    cmd(kPrecharge2, precharge2, 1);

    const uint8_t remap[] = {0x74};  // 65K color, split COM, RGB order, horizontal increment.
    cmd(kSetRemap, remap, 1);

    const uint8_t startline[] = {static_cast<uint8_t>(GBA_DISPLAY_SSD1351_STARTLINE)};
    cmd(kStartLine, startline, 1);

    cmd(kDisplayOn);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(kTag, "SSD1351 initialized (%dx%d, RGB565)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

static esp_err_t set_window_and_write(int x, int y, int w, int h, const void* rgb565_data) {
    const uint8_t xs = static_cast<uint8_t>(x);
    const uint8_t xe = static_cast<uint8_t>(x + w - 1);
    const uint8_t ys = static_cast<uint8_t>(y);
    const uint8_t ye = static_cast<uint8_t>(y + h - 1);

    const uint8_t caset[] = {xs, xe};
    cmd(kSetColumn, caset, 2);
    const uint8_t raset[] = {ys, ye};
    cmd(kSetRow, raset, 2);

    return tx_pixels(kWriteRam, rgb565_data, static_cast<size_t>(w) * h);
}

#else
#error Unsupported GBA_DISPLAY_DRIVER
#endif

esp_err_t display_init(void) {
    const uint64_t output_mask = gpio_mask(DISPLAY_PIN_RST) | gpio_mask(DISPLAY_PIN_BL);
    if (output_mask != 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = output_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), kTag, "gpio_config failed");
    }

#if GBA_DISPLAY_SWAP_BYTES
    s_swap_pixels = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
    s_swap_buf = static_cast<uint16_t*>(
        heap_caps_malloc(s_swap_pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!s_swap_buf) {
        ESP_LOGW(kTag, "internal DMA swap buffer failed, trying DMA-capable heap");
        s_swap_buf = static_cast<uint16_t*>(
            heap_caps_malloc(s_swap_pixels * sizeof(uint16_t), MALLOC_CAP_DMA));
    }
    if (!s_swap_buf) {
        ESP_LOGE(kTag, "failed to allocate %u-byte display swap buffer",
                 static_cast<unsigned>(s_swap_pixels * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }
#endif

    s_color_done = xSemaphoreCreateBinary();
    if (!s_color_done) {
        ESP_LOGE(kTag, "failed to allocate display completion semaphore");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = DISPLAY_PIN_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        kTag, "spi_bus_initialize failed");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_PIN_CS;
    io_config.dc_gpio_num = DISPLAY_PIN_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = DISPLAY_SPI_CLK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = color_trans_done_cb;
    io_config.user_ctx = s_color_done;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.dc_low_on_data = 0;
    io_config.flags.octal_mode = 0;
    io_config.flags.lsb_first = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &s_io),
        kTag, "esp_lcd_new_panel_io_spi failed");

    panel_init();
    display_backlight(true);
    return ESP_OK;
}

esp_err_t display_draw(int x, int y, int w, int h, const void* rgb565_data) {
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rgb565_data || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    return set_window_and_write(x, y, w, h, rgb565_data);
}

void display_backlight(bool on) {
    if (valid_gpio(DISPLAY_PIN_BL)) {
        gpio_set_level(DISPLAY_PIN_BL, on ? 1 : 0);
    }
}
