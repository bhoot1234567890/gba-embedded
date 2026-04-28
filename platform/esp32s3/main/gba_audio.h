#pragma once

#include "gba_board_profile.h"

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * Hardware-agnostic I2S audio output for ESP32-S3.
 *
 * Uses I2S standard (Philips) mode — compatible with:
 *   MAX98357A, PCM5102A, UDA1334A, PT8211, CS4344, etc.
 *
 * Wire your codec's BCK/BCLK, WS/LRCK, and DIN/DOUT to the GPIOs below.
 * Set AUDIO_PIN_* to GPIO_NUM_NC to disable audio output entirely.
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
#define AUDIO_DMA_BUF_COUNT  4        /* Number of DMA buffers */
#endif
#ifndef AUDIO_DMA_BUF_LEN
#define AUDIO_DMA_BUF_LEN   512       /* Samples per DMA buffer */
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
 * Blocks until all samples are written (natural backpressure).
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
