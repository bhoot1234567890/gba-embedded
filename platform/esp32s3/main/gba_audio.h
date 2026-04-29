#pragma once

#include "gba_board_profile.h"

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * Lightweight I2S audio output for ESP32-S3.
 *
 * Default target: MAX98357A I2S mono Class-D amplifier.
 * The MAX98357A does not need MCLK, so the ESP32-S3 only drives:
 *   BCLK, LRCLK/WS, and DOUT.
 *
 * Set any AUDIO_PIN_* to GPIO_NUM_NC to disable audio output entirely.
 */

/* ── GPIO pins (fill in your wiring) ── */
#ifndef AUDIO_PIN_BCK
#define AUDIO_PIN_BCK   GBA_AUDIO_PIN_BCK   /* Bit clock */
#endif
#ifndef AUDIO_PIN_WS
#define AUDIO_PIN_WS    GBA_AUDIO_PIN_WS    /* Word select / LRCK */
#endif
#ifndef AUDIO_PIN_DOUT
#define AUDIO_PIN_DOUT  GBA_AUDIO_PIN_DOUT  /* Serial data out */
#endif

/* ── Audio config ── */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE  32768      /* GBA native (~32.768 kHz) */
#endif
#ifndef AUDIO_DMA_BUF_COUNT
#define AUDIO_DMA_BUF_COUNT  3        /* Small DMA footprint, enough for frame jitter */
#endif
#ifndef AUDIO_DMA_BUF_LEN
#define AUDIO_DMA_BUF_LEN   256       /* Stereo frames per DMA buffer */
#endif
#ifndef AUDIO_WRITE_TIMEOUT_MS
#define AUDIO_WRITE_TIMEOUT_MS GBA_AUDIO_WRITE_TIMEOUT_MS
#endif

#ifndef AUDIO_AMP_NAME
#define AUDIO_AMP_NAME "MAX98357A"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize I2S peripheral for audio output.
 * Returns ESP_ERR_NOT_SUPPORTED if AUDIO_PIN_BCK is GPIO_NUM_NC (disabled).
 */
esp_err_t audio_init(void);

/**
 * Write interleaved stereo s16 samples to the I2S DMA buffer.
 * Uses AUDIO_WRITE_TIMEOUT_MS; default is 0 so audio can drop instead of
 * blocking the emulator frame loop.
 * @param samples  Pointer to interleaved L/R s16 sample pairs.
 * @param count    Total number of s16 values (samples * 2 for stereo).
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if audio disabled.
 */
esp_err_t audio_write(const int16_t* samples, size_t count);

/**
 * Tear down the I2S peripheral and free resources.
 */
void audio_deinit(void);

/**
 * @return true if audio was successfully initialized.
 */
bool audio_enabled(void);

#ifdef __cplusplus
}
#endif
