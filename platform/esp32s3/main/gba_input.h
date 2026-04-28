#pragma once

#include "gba_board_profile.h"

#include "driver/gpio.h"
#include "esp_err.h"

#include <stdint.h>

/*
 * Time-based per-key integrator debounce (ZMK-style) for ESP32-S3.
 *
 * Each button maps to one GPIO pin. Buttons are active-low
 * (pressed = GPIO reads 0). Internal pull-ups are enabled.
 *
 * A 1 ms hardware timer polls raw GPIO and runs the integrator:
 *   mismatch increments counter, match decrements it.
 *   State flips when counter reaches press_ms or release_ms.
 *
 * Set any pin to GPIO_NUM_NC to leave that button always released.
 * Set ALL pins to GPIO_NUM_NC to disable input entirely (returns 0x03FF).
 *
 * Tuning via kconfig / sdkconfig:
 *   CONFIG_GBA_INPUT_DEBOUNCE_PRESS_MS  (default 1)
 *   CONFIG_GBA_INPUT_DEBOUNCE_RELEASE_MS(default 5)
 */

/* ── GPIO pins (fill in your wiring) ── */
#ifndef INPUT_PIN_A
#define INPUT_PIN_A       GBA_INPUT_PIN_A
#endif
#ifndef INPUT_PIN_B
#define INPUT_PIN_B       GBA_INPUT_PIN_B
#endif
#ifndef INPUT_PIN_SELECT
#define INPUT_PIN_SELECT  GBA_INPUT_PIN_SELECT
#endif
#ifndef INPUT_PIN_START
#define INPUT_PIN_START   GBA_INPUT_PIN_START
#endif
#ifndef INPUT_PIN_RIGHT
#define INPUT_PIN_RIGHT   GBA_INPUT_PIN_RIGHT
#endif
#ifndef INPUT_PIN_LEFT
#define INPUT_PIN_LEFT    GBA_INPUT_PIN_LEFT
#endif
#ifndef INPUT_PIN_UP
#define INPUT_PIN_UP      GBA_INPUT_PIN_UP
#endif
#ifndef INPUT_PIN_DOWN
#define INPUT_PIN_DOWN    GBA_INPUT_PIN_DOWN
#endif
#ifndef INPUT_PIN_R
#define INPUT_PIN_R       GBA_INPUT_PIN_R
#endif
#ifndef INPUT_PIN_L
#define INPUT_PIN_L       GBA_INPUT_PIN_L
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize GPIO pins for button input.
 * Returns ESP_ERR_NOT_SUPPORTED if all pins are GPIO_NUM_NC.
 */
esp_err_t input_init(void);

/**
 * Read current button state in GBA KEYINPUT format.
 * Bit = 1 means released, bit = 0 means pressed.
 * Bit layout: [9:L][8:R][7:Down][6:Up][5:Left][4:Right][3:Start][2:Select][1:B][0:A]
 */
uint16_t input_read_keys(void);

/**
 * @return true if input was successfully initialized with at least one GPIO.
 */
bool input_enabled(void);

#ifdef __cplusplus
}
#endif
