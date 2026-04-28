#pragma once

#include "gba_board_profile.h"

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * SPI display driver for ESP32-S3.
 * Supports ST7735 128x128 and SSD1351 128x96/128x128 through board macros.
 * Uses ESP-IDF SPI DMA for pixel transfers.
 */

/* ── GPIO pins (board profile defaults, override as needed) ── */
#define DISPLAY_PIN_SCLK   GBA_DISPLAY_PIN_SCLK
#define DISPLAY_PIN_MOSI   GBA_DISPLAY_PIN_MOSI
#define DISPLAY_PIN_DC     GBA_DISPLAY_PIN_DC
#define DISPLAY_PIN_CS     GBA_DISPLAY_PIN_CS
#define DISPLAY_PIN_RST    GBA_DISPLAY_PIN_RST
#define DISPLAY_PIN_BL     GBA_DISPLAY_PIN_BL

/* ── Display dimensions ── */
#define DISPLAY_WIDTH      GBA_DISPLAY_WIDTH
#define DISPLAY_HEIGHT     GBA_DISPLAY_HEIGHT

/* ── ST7735 green-tab offset (well-known quirk) ──
 * Many 128x128 ST7735 modules have visible area offset by a few pixels.
 * Set to 0,0 if your display doesn't need offset.
 */
#define DISPLAY_COL_OFFSET  GBA_DISPLAY_COL_OFFSET
#define DISPLAY_ROW_OFFSET  GBA_DISPLAY_ROW_OFFSET

/* ── SPI config ── */
#define DISPLAY_SPI_HOST    SPI2_HOST
#define DISPLAY_SPI_CLK_HZ  GBA_DISPLAY_SPI_CLK_HZ

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
esp_err_t display_draw(int x, int y, int w, int h, const void* rgb565_data);
void display_backlight(bool on);

#ifdef __cplusplus
}
#endif
