/*
 * Time-based per-key integrator debounce (ZMK-style).
 * A 1 ms esp_timer polls raw GPIO. Counter increments on mismatch,
 * decrements on match. When counter reaches the configured threshold
 * (press_ms or release_ms), the debounced state flips.
 *
 * input_read_keys() returns the debounced state directly —
 * no additional computation in the hot path.
 */

#include "gba_input.h"

#include "esp_timer.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char* kTag = "gba_input";

/* Button-to-GPIO mapping table, indexed by GBA KEYINPUT bit position */
static const gpio_num_t s_pin_map[10] = {
    INPUT_PIN_A,       /* bit 0 */
    INPUT_PIN_B,       /* bit 1 */
    INPUT_PIN_SELECT,  /* bit 2 */
    INPUT_PIN_START,   /* bit 3 */
    INPUT_PIN_RIGHT,   /* bit 4 */
    INPUT_PIN_LEFT,    /* bit 5 */
    INPUT_PIN_UP,      /* bit 6 */
    INPUT_PIN_DOWN,    /* bit 7 */
    INPUT_PIN_R,       /* bit 8 */
    INPUT_PIN_L,       /* bit 9 */
};

/* ── debounce configuration ── */

#ifndef CONFIG_GBA_INPUT_DEBOUNCE_PRESS_MS
#define CONFIG_GBA_INPUT_DEBOUNCE_PRESS_MS 1
#endif
#ifndef CONFIG_GBA_INPUT_DEBOUNCE_RELEASE_MS
#define CONFIG_GBA_INPUT_DEBOUNCE_RELEASE_MS 5
#endif

static constexpr uint16_t kDebouncePressMs = CONFIG_GBA_INPUT_DEBOUNCE_PRESS_MS;
static constexpr uint16_t kDebounceReleaseMs = CONFIG_GBA_INPUT_DEBOUNCE_RELEASE_MS;
static constexpr uint16_t kScanPeriodMs = 1;

/* ── per-key state ── */

static constexpr int kMaxKeys = 10;

static struct KeyState {
    bool pressed;     /* debounced state */
    uint16_t counter; /* accumulated unstable time (ms) */
} s_keys[kMaxKeys];

static volatile uint16_t s_debounced_keys = 0x03FF;  /* all released */
static bool s_initialized = false;
static int s_active_count = 0;

static esp_timer_handle_t s_timer;

/* ── saturating 16-bit add ── */

static inline uint16_t sat_add_u16(uint16_t a, uint16_t b, uint16_t max) {
    if (a > max - b) return max;
    return a + b;
}

/* ── 1 ms timer callback: read GPIO, run integrator ── */

static void input_timer_callback(void* /*arg*/) {
    /* Read raw GPIO in one pass. Active-low: GPIO = 0 when pressed.
     * Build raw mask in GBA KEYINPUT format (1 = released, 0 = pressed). */
    uint16_t raw = 0x03FF;

    for (int i = 0; i < kMaxKeys; ++i) {
        if (s_pin_map[i] == GPIO_NUM_NC) {
            continue;
        }
        if (gpio_get_level(s_pin_map[i]) == 0) {
            raw &= ~(1u << i);
        }
    }

    /* Update integrator per key */
    for (int i = 0; i < kMaxKeys; ++i) {
        if (s_pin_map[i] == GPIO_NUM_NC) {
            continue;
        }

        bool raw_active = !(raw & (1u << i));  /* true = pressed */

        if (raw_active == s_keys[i].pressed) {
            /* Match: decay counter toward 0 */
            if (s_keys[i].counter > kScanPeriodMs) {
                s_keys[i].counter -= kScanPeriodMs;
            } else {
                s_keys[i].counter = 0;
            }
        } else {
            /* Mismatch: accumulate evidence */
            uint16_t threshold = s_keys[i].pressed ? kDebounceReleaseMs : kDebouncePressMs;

            /* Eager path: threshold 0 → flip immediately */
            if (threshold == 0) {
                s_keys[i].pressed = raw_active;
                s_keys[i].counter = 0;
            } else {
                s_keys[i].counter = sat_add_u16(s_keys[i].counter, kScanPeriodMs, threshold);

                if (s_keys[i].counter >= threshold) {
                    s_keys[i].pressed = raw_active;
                    s_keys[i].counter = 0;
                }
            }
        }
    }

    /* Build debounced KEYINPUT value */
    uint16_t debounced = 0x03FF;
    for (int i = 0; i < kMaxKeys; ++i) {
        if (s_pin_map[i] == GPIO_NUM_NC) {
            continue;
        }
        if (s_keys[i].pressed) {
            debounced &= ~(1u << i);
        }
    }

    s_debounced_keys = debounced;
}

/* ── public API ── */

esp_err_t input_init(void) {
    s_active_count = 0;

    for (int i = 0; i < kMaxKeys; ++i) {
        if (s_pin_map[i] == GPIO_NUM_NC) {
            continue;
        }

        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_pin_map[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        esp_err_t ret = gpio_config(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to configure GPIO %d for button %d: %s",
                     s_pin_map[i], i, esp_err_to_name(ret));
            return ret;
        }
        ++s_active_count;
    }

    if (s_active_count == 0) {
        ESP_LOGW(kTag, "Input disabled (all pins = GPIO_NUM_NC)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Reset per-key state */
    for (int i = 0; i < kMaxKeys; ++i) {
        s_keys[i].pressed = false;
        s_keys[i].counter = 0;
    }
    s_debounced_keys = 0x03FF;

    /* Start 1 ms periodic timer */
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &input_timer_callback;
    timer_args.name = "gba_input";

    esp_err_t ret = esp_timer_create(&timer_args, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to create debounce timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_timer, kScanPeriodMs * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start debounce timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_timer);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(kTag, "Input: %d buttons, press=%ums release=%ums scan=%ums",
             s_active_count, kDebouncePressMs, kDebounceReleaseMs, kScanPeriodMs);
    return ESP_OK;
}

uint16_t input_read_keys(void) {
    if (!s_initialized) {
        return 0x03FF;
    }
    return s_debounced_keys;
}

bool input_enabled(void) {
    return s_initialized;
}
