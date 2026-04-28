#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"

/*
 * Board-profile defaults for ESP32-S3 runtime.
 * Override any macro from the build system or a board-specific header.
 */

#define GBA_STORAGE_MODE_SDMMC 1
#define GBA_STORAGE_MODE_SDSPI 2

#ifndef GBA_STORAGE_MODE
#define GBA_STORAGE_MODE GBA_STORAGE_MODE_SDSPI
#endif

/* Display (ST7735 over SPI) */
#ifndef GBA_DISPLAY_PIN_SCLK
#define GBA_DISPLAY_PIN_SCLK GPIO_NUM_4
#endif
#ifndef GBA_DISPLAY_PIN_MOSI
#define GBA_DISPLAY_PIN_MOSI GPIO_NUM_5
#endif
#ifndef GBA_DISPLAY_PIN_DC
#define GBA_DISPLAY_PIN_DC GPIO_NUM_6
#endif
#ifndef GBA_DISPLAY_PIN_CS
#define GBA_DISPLAY_PIN_CS GPIO_NUM_7
#endif
#ifndef GBA_DISPLAY_PIN_RST
#define GBA_DISPLAY_PIN_RST GPIO_NUM_8
#endif
#ifndef GBA_DISPLAY_PIN_BL
#define GBA_DISPLAY_PIN_BL GPIO_NUM_9
#endif

/* SDMMC 4-bit mode */
#ifndef GBA_SDMMC_PIN_CLK
#define GBA_SDMMC_PIN_CLK GPIO_NUM_14
#endif
#ifndef GBA_SDMMC_PIN_CMD
#define GBA_SDMMC_PIN_CMD GPIO_NUM_15
#endif
#ifndef GBA_SDMMC_PIN_D0
#define GBA_SDMMC_PIN_D0 GPIO_NUM_2
#endif
#ifndef GBA_SDMMC_PIN_D1
#define GBA_SDMMC_PIN_D1 GPIO_NUM_17
#endif
#ifndef GBA_SDMMC_PIN_D2
#define GBA_SDMMC_PIN_D2 GPIO_NUM_12
#endif
#ifndef GBA_SDMMC_PIN_D3
#define GBA_SDMMC_PIN_D3 GPIO_NUM_13
#endif

/* SDSPI mode */
#ifndef GBA_SDSPI_HOST
#define GBA_SDSPI_HOST SPI3_HOST
#endif
#ifndef GBA_SDSPI_PIN_MISO
#define GBA_SDSPI_PIN_MISO GPIO_NUM_12
#endif
#ifndef GBA_SDSPI_PIN_MOSI
#define GBA_SDSPI_PIN_MOSI GPIO_NUM_13
#endif
#ifndef GBA_SDSPI_PIN_SCLK
#define GBA_SDSPI_PIN_SCLK GPIO_NUM_14
#endif
#ifndef GBA_SDSPI_PIN_CS
#define GBA_SDSPI_PIN_CS GPIO_NUM_15
#endif

/* Input: default to disabled until board wiring is finalized */
#ifndef GBA_INPUT_PIN_A
#define GBA_INPUT_PIN_A GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_B
#define GBA_INPUT_PIN_B GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_SELECT
#define GBA_INPUT_PIN_SELECT GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_START
#define GBA_INPUT_PIN_START GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_RIGHT
#define GBA_INPUT_PIN_RIGHT GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_LEFT
#define GBA_INPUT_PIN_LEFT GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_UP
#define GBA_INPUT_PIN_UP GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_DOWN
#define GBA_INPUT_PIN_DOWN GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_R
#define GBA_INPUT_PIN_R GPIO_NUM_NC
#endif
#ifndef GBA_INPUT_PIN_L
#define GBA_INPUT_PIN_L GPIO_NUM_NC
#endif

/* Audio: default to disabled until codec wiring is finalized */
#ifndef GBA_AUDIO_PIN_BCK
#define GBA_AUDIO_PIN_BCK GPIO_NUM_NC
#endif
#ifndef GBA_AUDIO_PIN_WS
#define GBA_AUDIO_PIN_WS GPIO_NUM_NC
#endif
#ifndef GBA_AUDIO_PIN_DOUT
#define GBA_AUDIO_PIN_DOUT GPIO_NUM_NC
#endif

