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
#include "esp_log.h"
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

/* ── Private constants ───────────────────────────────────────────── */

static const char *TAG = "led_ctrl";

/* RMT resolution – 10 MHz is the recommended value for WS2812B */
#define RMT_LED_STRIP_RESOLUTION_HZ (10 * 1000 * 1000)

/* ── Private state ───────────────────────────────────────────────── */

static led_strip_handle_t s_strip   = NULL;
static bool               s_is_on   = false;
static uint8_t            s_brightness = CONFIG_LED_STRIP_DEFAULT_BRIGHTNESS;
static uint8_t            s_red     = 255;
static uint8_t            s_green   = 255;
static uint8_t            s_blue    = 255;

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Apply brightness scaling to a single colour component.
 */
static inline uint8_t scale(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness) / 255);
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
    s_is_on = true;
    ESP_LOGI(TAG, "LED strip ON (R=%d G=%d B=%d, brightness=%d)",
             s_red, s_green, s_blue, s_brightness);
    return apply_all();
}

esp_err_t led_controller_off(void)
{
    s_is_on = false;
    ESP_LOGI(TAG, "LED strip OFF");
    return apply_all();
}

esp_err_t led_controller_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    ESP_LOGI(TAG, "Brightness set to %d", brightness);
    if (s_is_on) {
        return apply_all();
    }
    return ESP_OK;
}

esp_err_t led_controller_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    s_red   = red;
    s_green = green;
    s_blue  = blue;
    ESP_LOGI(TAG, "Colour set to R=%d G=%d B=%d", red, green, blue);
    if (s_is_on) {
        return apply_all();
    }
    return ESP_OK;
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
    return s_brightness;
}

void led_controller_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (red)   *red   = s_red;
    if (green) *green = s_green;
    if (blue)  *blue  = s_blue;
}

bool led_controller_is_on(void)
{
    return s_is_on;
}

uint16_t led_controller_get_num_leds(void)
{
    return CONFIG_LED_STRIP_NUM_LEDS;
}
