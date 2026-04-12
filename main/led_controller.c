/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - LED Strip Controller implementation
 * Drives a WS2812B addressable LED strip via the RMT peripheral
 * using the espressif/led_strip component.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "led_controller.h"

/* ── Configuration from Kconfig ──────────────────────────────────── */

#ifndef CONFIG_LED_STRIP_GPIO
#define CONFIG_LED_STRIP_GPIO 8
#endif

#ifndef CONFIG_LED_STRIP_NUM_LEDS
#define CONFIG_LED_STRIP_NUM_LEDS 105
#endif

#ifndef CONFIG_LED_STRIP_DEFAULT_BRIGHTNESS
#define CONFIG_LED_STRIP_DEFAULT_BRIGHTNESS 128
#endif

#ifndef CONFIG_LED_RAMP_DURATION_SEC
#define CONFIG_LED_RAMP_DURATION_SEC 30
#endif

/* ── Private constants ───────────────────────────────────────────── */

static const char *TAG = "led_ctrl";

/* RMT resolution – 10 MHz is the recommended value for WS2812B */
#define RMT_LED_STRIP_RESOLUTION_HZ (10 * 1000 * 1000)

/* ── Gamma correction (gamma ≈ 2.2) ──────────────────────────────── */

/**
 * Look-up table that maps a linear 0-255 brightness value to a
 * gamma-corrected output.  This gives perceptually smooth fading
 * because human vision responds non-linearly to luminance.
 *
 * Generated with: round(pow(i / 255.0, 2.2) * 255) for i in 0..255
 */
static const uint8_t GAMMA_LUT[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,
     12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,
     20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
     30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,  39,  39,  40,  41,
     42,  43,  43,  44,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
     73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,
     91,  93,  94,  95,  97,  98,  99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};

/* ── Private state ───────────────────────────────────────────────── */

static led_strip_handle_t  s_strip   = NULL;
static SemaphoreHandle_t   s_mutex   = NULL;   /* protects colour/brightness state */
static bool                s_is_on   = false;
static uint8_t             s_brightness = CONFIG_LED_STRIP_DEFAULT_BRIGHTNESS;
static uint8_t             s_red     = 255;
static uint8_t             s_green   = 255;
static uint8_t             s_blue    = 255;

/* ── Acclimatization ramp state ──────────────────────────────────── */

#define RAMP_TICK_MS  30   /* ~33 fps, matches scene tick rate */

static esp_timer_handle_t s_ramp_timer  = NULL;
static uint8_t  s_ramp_start_br  = 0;   /* brightness at ramp start  */
static uint8_t  s_ramp_end_br    = 0;   /* brightness at ramp end    */
static uint32_t s_ramp_total_ms  = 0;   /* total ramp duration       */
static uint32_t s_ramp_elapsed   = 0;   /* elapsed ramp time         */
static bool     s_ramp_active    = false;
static bool     s_ramp_off       = false; /* true if ramping to OFF   */

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Apply brightness scaling with gamma correction to a single
 *        colour component.
 *
 * The brightness value is passed through a gamma 2.2 look-up table
 * so that perceived brightness ramps smoothly for the human eye.
 */
static inline uint8_t scale(uint8_t value, uint8_t brightness)
{
    uint8_t gamma_br = GAMMA_LUT[brightness];
    return (uint8_t)(((uint16_t)value * gamma_br) / 255);
}

/**
 * @brief Write the current colour (scaled by brightness) to every LED
 *        and refresh the strip.
 */
static esp_err_t apply_all(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t r = s_is_on ? scale(s_red,   s_brightness) : 0;
    uint8_t g = s_is_on ? scale(s_green, s_brightness) : 0;
    uint8_t b = s_is_on ? scale(s_blue,  s_brightness) : 0;

    for (int i = 0; i < CONFIG_LED_STRIP_NUM_LEDS; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, i, r, g, b);
        if (err != ESP_OK) {
            return err;
        }
    }

    return led_strip_refresh(s_strip);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t led_controller_init(void)
{
    ESP_LOGI(TAG, "Initialising WS2812B strip – GPIO %d, %d LEDs",
             CONFIG_LED_STRIP_GPIO, CONFIG_LED_STRIP_NUM_LEDS);

    /* Create mutex for thread-safe state access */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LED mutex");
        return ESP_ERR_NO_MEM;
    }

    /* LED strip general configuration */
    led_strip_config_t strip_config = {
        .strip_gpio_num   = CONFIG_LED_STRIP_GPIO,
        .max_leds         = CONFIG_LED_STRIP_NUM_LEDS,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    /* RMT backend configuration */
    led_strip_rmt_config_t rmt_config = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = RMT_LED_STRIP_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = true, /* ESP32-P4 supports DMA for RMT */
        },
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start with strip off */
    s_is_on = false;
    ret = led_strip_clear(s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LED strip initialised (brightness=%d)", s_brightness);
    return ESP_OK;
}

esp_err_t led_controller_on(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_is_on = true;
    ESP_LOGI(TAG, "LED strip ON (R=%d G=%d B=%d, brightness=%d)",
             s_red, s_green, s_blue, s_brightness);
    esp_err_t ret = apply_all();
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t led_controller_off(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_is_on = false;
    ESP_LOGI(TAG, "LED strip OFF");
    esp_err_t ret = apply_all();
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t led_controller_set_brightness(uint8_t brightness)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_brightness = brightness;
    ESP_LOGI(TAG, "Brightness set to %d", brightness);
    esp_err_t ret = ESP_OK;
    if (s_is_on) {
        ret = apply_all();
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t led_controller_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_red   = red;
    s_green = green;
    s_blue  = blue;
    ESP_LOGI(TAG, "Color set to R=%d G=%d B=%d", red, green, blue);
    esp_err_t ret = ESP_OK;
    if (s_is_on) {
        ret = apply_all();
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t led_controller_set_pixel(uint16_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= CONFIG_LED_STRIP_NUM_LEDS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t r = scale(red,   s_brightness);
    uint8_t g = scale(green, s_brightness);
    uint8_t b = scale(blue,  s_brightness);

    return led_strip_set_pixel(s_strip, index, r, g, b);
}

esp_err_t led_controller_refresh(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_refresh(s_strip);
}

uint8_t led_controller_get_brightness(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t br = s_brightness;
    xSemaphoreGive(s_mutex);
    return br;
}

void led_controller_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (red)   *red   = s_red;
    if (green) *green = s_green;
    if (blue)  *blue  = s_blue;
    xSemaphoreGive(s_mutex);
}

bool led_controller_is_on(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool on = s_is_on;
    xSemaphoreGive(s_mutex);
    return on;
}

uint16_t led_controller_get_num_leds(void)
{
    return CONFIG_LED_STRIP_NUM_LEDS;
}

/* ── Acclimatization ramp ────────────────────────────────────────── */

/**
 * @brief Periodic timer callback that drives the brightness ramp.
 *
 * Note: esp_timer callbacks run in the esp_timer FreeRTOS task
 * context (ESP_TIMER_TASK dispatch, the default), not in ISR
 * context, so mutex usage and LED strip operations are safe.
 */
static void ramp_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_ramp_active) {
        xSemaphoreGive(s_mutex);
        return;
    }

    s_ramp_elapsed += RAMP_TICK_MS;
    float t = (float)s_ramp_elapsed / (float)s_ramp_total_ms;
    if (t >= 1.0f) {
        t = 1.0f;
    }

    /* Linearly interpolate brightness */
    s_brightness = (uint8_t)((float)s_ramp_start_br +
                             ((float)s_ramp_end_br - (float)s_ramp_start_br) * t);
    apply_all();

    if (t >= 1.0f) {
        s_ramp_active = false;
        esp_timer_stop(s_ramp_timer);

        if (s_ramp_off) {
            s_is_on = false;
            apply_all();
        }

        ESP_LOGI(TAG, "Fade ramp complete (brightness=%d, on=%d)",
                 s_brightness, s_is_on);
    }

    xSemaphoreGive(s_mutex);
}

esp_err_t led_controller_fade_on(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return led_controller_on();
    }

    /* Create timer on first use */
    if (s_ramp_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = ramp_timer_cb,
            .name     = "led_ramp",
        };
        esp_err_t err = esp_timer_create(&args, &s_ramp_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ramp timer: %s",
                     esp_err_to_name(err));
            return led_controller_on();   /* fallback to instant */
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Stop any running ramp */
    if (s_ramp_active) {
        esp_timer_stop(s_ramp_timer);
        s_ramp_active = false;
    }

    s_is_on = true;
    uint8_t target = s_brightness;
    s_brightness = 0;
    apply_all();

    s_ramp_start_br = 0;
    s_ramp_end_br   = target;
    s_ramp_total_ms = duration_ms;
    s_ramp_elapsed  = 0;
    s_ramp_off      = false;
    s_ramp_active   = true;

    ESP_LOGI(TAG, "Fade ON: 0 -> %d over %"PRIu32" ms", target, duration_ms);

    xSemaphoreGive(s_mutex);

    return esp_timer_start_periodic(s_ramp_timer,
                                    (uint64_t)RAMP_TICK_MS * 1000);
}

esp_err_t led_controller_fade_off(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return led_controller_off();
    }

    /* Create timer on first use */
    if (s_ramp_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = ramp_timer_cb,
            .name     = "led_ramp",
        };
        esp_err_t err = esp_timer_create(&args, &s_ramp_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ramp timer: %s",
                     esp_err_to_name(err));
            return led_controller_off();   /* fallback to instant */
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Stop any running ramp */
    if (s_ramp_active) {
        esp_timer_stop(s_ramp_timer);
        s_ramp_active = false;
    }

    if (!s_is_on) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;   /* already off */
    }

    s_ramp_start_br = s_brightness;
    s_ramp_end_br   = 0;
    s_ramp_total_ms = duration_ms;
    s_ramp_elapsed  = 0;
    s_ramp_off      = true;
    s_ramp_active   = true;

    ESP_LOGI(TAG, "Fade OFF: %d -> 0 over %"PRIu32" ms",
             s_brightness, duration_ms);

    xSemaphoreGive(s_mutex);

    return esp_timer_start_periodic(s_ramp_timer,
                                    (uint64_t)RAMP_TICK_MS * 1000);
}

bool led_controller_is_fading(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool fading = s_ramp_active;
    xSemaphoreGive(s_mutex);
    return fading;
}
