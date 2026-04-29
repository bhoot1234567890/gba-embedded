/*
 * I2S audio output for ESP32-S3 (ESP-IDF 5.x new driver API).
 *
 * Uses i2s_std (standard / Philips mode), 16-bit stereo.
 * Tuned for MAX98357A: no MCLK, just BCLK/LRCLK/DOUT.
 */

#include "gba_audio.h"

#include <algorithm>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char* kTag = "gba_audio";
static i2s_chan_handle_t s_tx_handle = NULL;
static bool s_initialized = false;

esp_err_t audio_init(void) {
    if (AUDIO_PIN_BCK == GPIO_NUM_NC || AUDIO_PIN_WS == GPIO_NUM_NC || AUDIO_PIN_DOUT == GPIO_NUM_NC) {
        ESP_LOGW(kTag, "Audio disabled (%s pins BCK=%d WS=%d DOUT=%d)",
                 AUDIO_AMP_NAME, AUDIO_PIN_BCK, AUDIO_PIN_WS, AUDIO_PIN_DOUT);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Allocate I2S TX channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN;
    chan_cfg.auto_clear = true;  /* zero-fill on underrun */

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure standard Philips I2S. MAX98357A derives its clock from BCLK/LRCLK. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)AUDIO_PIN_BCK,
            .ws   = (gpio_num_t)AUDIO_PIN_WS,
            .dout = (gpio_num_t)AUDIO_PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(kTag, "%s I2S audio initialized: %d Hz, 16-bit stereo, DMA=%dx%d, BCK=%d WS=%d DOUT=%d",
             AUDIO_AMP_NAME, AUDIO_SAMPLE_RATE, AUDIO_DMA_BUF_COUNT, AUDIO_DMA_BUF_LEN,
             AUDIO_PIN_BCK, AUDIO_PIN_WS, AUDIO_PIN_DOUT);
    return ESP_OK;
}

esp_err_t audio_write(const int16_t* samples, size_t count) {
    if (!s_initialized || s_tx_handle == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const TickType_t timeout_ticks =
        AUDIO_WRITE_TIMEOUT_MS <= 0 ? 0 : pdMS_TO_TICKS(AUDIO_WRITE_TIMEOUT_MS);

    size_t offset = 0;
    while (offset < count) {
        const size_t samples_this_write = std::min(count - offset, static_cast<size_t>(AUDIO_DMA_BUF_LEN) * 2u);
        const size_t bytes = samples_this_write * sizeof(int16_t);
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_tx_handle, samples + offset, bytes, &bytes_written, timeout_ticks);
        if (ret == ESP_OK && bytes_written != bytes) {
            ret = ESP_ERR_TIMEOUT;
        }
        if (ret != ESP_OK) {
            if (ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(kTag, "i2s_channel_write error: %s (wrote %zu/%zu)",
                         esp_err_to_name(ret), bytes_written, bytes);
            }
            return ret;
        }
        offset += samples_this_write;
    }
    return ESP_OK;
}

void audio_deinit(void) {
    if (s_tx_handle != NULL) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        s_initialized = false;
        ESP_LOGI(kTag, "I2S audio deinitialized");
    }
}

bool audio_enabled(void) {
    return s_initialized;
}
