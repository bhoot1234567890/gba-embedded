/*
 * ST7735 128x128 SPI display driver for ESP32-S3.
 * Manual init sequence — ESP-IDF v5.5 has no built-in ST7735 driver.
 * Uses esp_lcd_panel_io_spi for DMA-backed transfers.
 */

#include "gba_display.h"

#include <cstring>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* kTag = "display";
static esp_lcd_panel_io_handle_t s_io = nullptr;

/* ── ST7735 command definitions ── */
enum St7735Cmd : uint8_t {
    kNop      = 0x00,
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

/* ── Helper: send command + parameters ── */
static void cmd(uint8_t command, const uint8_t* params = nullptr, int param_len = 0) {
    esp_lcd_panel_io_tx_param(s_io, command, params, param_len);
}

/* ── Hardware reset ── */
static void hardware_reset() {
    gpio_set_level(DISPLAY_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ── Init sequence ── */
static void st7735_init() {
    hardware_reset();

    /* Software reset */
    cmd(kSwReset);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Out of sleep */
    cmd(kSlpOut);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Frame rate control — normal mode: RTNA=0x01, FPA=0x2C, BPA=0x2D */
    const uint8_t frm1[] = {0x01, 0x2C, 0x2D};
    cmd(kFrmCtr1, frm1, 3);

    /* Frame rate — idle mode: RTNA=0x01, FPA=0x2C, BPA=0x2D */
    const uint8_t frm2[] = {0x01, 0x2C, 0x2D};
    cmd(kFrmCtr2, frm2, 3);

    /* Frame rate — partial mode: DOTC=0x01, FPA=0x2C, BPA=0x2D */
    const uint8_t frm3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    cmd(kFrmCtr3, frm3, 6);

    /* Display inversion ctrl: 1 line inversion */
    const uint8_t invctr[] = {0x07};
    cmd(kInvCtr, invctr, 1);

    /* Power control 1: AVDD=4.6V, GVDD=4.6V, GVCL=-4.6V, MODE=AUTO */
    const uint8_t pw1[] = {0xA2, 0x02, 0x84};
    cmd(kPwCtr1, pw1, 3);

    /* Power control 2: VGH25=2.4V, VGL=-10V, VGSEL=3*AVDD */
    const uint8_t pw2[] = {0xC5};
    cmd(kPwCtr2, pw2, 1);

    /* Power control 3: OpA=small, Boost frequency */
    const uint8_t pw3[] = {0x0A, 0x00};
    cmd(kPwCtr3, pw3, 2);

    /* Power control 4: BCLK divider */
    const uint8_t pw4[] = {0x8A, 0x2A};
    cmd(kPwCtr4, pw4, 2);

    /* Power control 5: BCLK divider */
    const uint8_t pw5[] = {0x8A, 0xEE};
    cmd(kPwCtr5, pw5, 2);

    /* VCOM voltage control */
    const uint8_t vm1[] = {0x0E};
    cmd(kVmCtr1, vm1, 1);

    /* Pixel format: 16-bit RGB565 */
    const uint8_t colmod[] = {0x05};
    cmd(kColMod, colmod, 1);

    /* Memory access control: no rotation, RGB order, no mirror */
    const uint8_t madctl[] = {0x00};
    cmd(kMadCtl, madctl, 1);

    /* Column address set (with green-tab offset) */
    const uint8_t caset[] = {
        0x00, static_cast<uint8_t>(DISPLAY_COL_OFFSET),
        0x00, static_cast<uint8_t>(DISPLAY_COL_OFFSET + DISPLAY_WIDTH - 1)
    };
    cmd(kCaseT, caset, 4);

    /* Row address set (with green-tab offset) */
    const uint8_t raset[] = {
        0x00, static_cast<uint8_t>(DISPLAY_ROW_OFFSET),
        0x00, static_cast<uint8_t>(DISPLAY_ROW_OFFSET + DISPLAY_HEIGHT - 1)
    };
    cmd(kRasT, raset, 4);

    /* Display inversion on — compensates for green tab color shift */
    cmd(kInvOn);

    /* Normal display mode */
    cmd(kNorOn);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Display on */
    cmd(kDispOn);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(kTag, "ST7735 initialized (128x128, RGB565, offset %d,%d)",
             DISPLAY_COL_OFFSET, DISPLAY_ROW_OFFSET);
}

/* ── Public API ── */

esp_err_t display_init(void) {
    /* Configure RST and BL as GPIO outputs */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DISPLAY_PIN_RST) | (1ULL << DISPLAY_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* Initialize SPI bus with DMA */
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Create LCD panel IO (SPI transport) */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISPLAY_PIN_DC,
        .cs_gpio_num = DISPLAY_PIN_CS,
        .pclk_hz = DISPLAY_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .flags = {
            .dc_as_cmd_phase = 0,
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .lsb_first = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &s_io));

    /* Send ST7735 init sequence */
    st7735_init();

    return ESP_OK;
}

esp_err_t display_draw(int x, int y, int w, int h, const void* rgb565_data) {
    if (!s_io) return ESP_ERR_INVALID_STATE;

    /* Set draw window */
    const uint8_t xs = static_cast<uint8_t>(DISPLAY_COL_OFFSET + x);
    const uint8_t xe = static_cast<uint8_t>(DISPLAY_COL_OFFSET + x + w - 1);
    const uint8_t ys = static_cast<uint8_t>(DISPLAY_ROW_OFFSET + y);
    const uint8_t ye = static_cast<uint8_t>(DISPLAY_ROW_OFFSET + y + h - 1);

    const uint8_t caset[] = {0x00, xs, 0x00, xe};
    cmd(kCaseT, caset, 4);
    const uint8_t raset[] = {0x00, ys, 0x00, ye};
    cmd(kRasT, raset, 4);

    /* Send pixel data via DMA */
    const size_t len = static_cast<size_t>(w) * h * sizeof(uint16_t);
    return esp_lcd_panel_io_tx_color(s_io, kRamWr, rgb565_data, len);
}

void display_backlight(bool on) {
    gpio_set_level(DISPLAY_PIN_BL, on ? 1 : 0);
}
