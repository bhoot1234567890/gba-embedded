#pragma once

#include "gba_board_profile.h"

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * ST7735 128x128 SPI display driver for ESP32-S3.
 * Uses ESP-IDF SPI DMA for pixel transfers.
 *
 * GPIO pin assignments — fill in for your wiring.
 * The ST7735 green-tab offset is configurable below.
 */

/* ── GPIO pins (board profile defaults, override as needed) ── */
#define DISPLAY_PIN_SCLK   GBA_DISPLAY_PIN_SCLK
#define DISPLAY_PIN_MOSI   GBA_DISPLAY_PIN_MOSI
#define DISPLAY_PIN_DC     GBA_DISPLAY_PIN_DC
#define DISPLAY_PIN_CS     GBA_DISPLAY_PIN_CS
#define DISPLAY_PIN_RST    GBA_DISPLAY_PIN_RST
#define DISPLAY_PIN_BL     GBA_DISPLAY_PIN_BL

/* ── Display dimensions ── */
#define DISPLAY_WIDTH      128
#define DISPLAY_HEIGHT     128

/* ── ST7735 green-tab offset (well-known quirk) ──
 * Many 128x128 ST7735 modules have visible area offset by a few pixels.
 * Set to 0,0 if your display doesn't need offset.
 */
#define DISPLAY_COL_OFFSET  2
#define DISPLAY_ROW_OFFSET  1

/* ── SPI config ── */
#define DISPLAY_SPI_HOST    SPI2_HOST
#define DISPLAY_SPI_CLK_HZ  (26 * 1000 * 1000)  /* 26MHz — safe for ST7735 */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
esp_err_t display_draw(int x, int y, int w, int h, const void* rgb565_data);
void display_backlight(bool on);

#ifdef __cplusplus
}
#endif
