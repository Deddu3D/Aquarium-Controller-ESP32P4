/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Feeding Mode implementation
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "feeding_mode.h"
#include "relay_controller.h"
#include "led_controller.h"
#include "telegram_notify.h"

static const char *TAG = "feeding";

/* ── NVS keys ────────────────────────────────────────────────────── */

#define NVS_NAMESPACE      "feeding"
#define NVS_KEY_RELAY      "relay_idx"
#define NVS_KEY_DURATION   "duration"
#define NVS_KEY_DIM        "dim_lights"
#define NVS_KEY_DIM_BR     "dim_bright"

/* ── Defaults ────────────────────────────────────────────────────── */

#define DEFAULT_RELAY_INDEX    0
#define DEFAULT_DURATION_MIN   10
#define DEFAULT_DIM_LIGHTS     false
#define DEFAULT_DIM_BRIGHTNESS 60

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex         = NULL;
static feeding_config_t  s_config;
static bool              s_active        = false;
static time_t            s_end_time      = 0;    /* UNIX epoch seconds */
static bool              s_relay_was_on  = false; /* relay state before feeding */
static uint8_t           s_prev_bright   = 255;   /* LED brightness before feeding */
static bool              s_prev_led_on   = false;  /* LED on/off state before feeding */

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    /* Defaults */
    s_config.relay_index    = DEFAULT_RELAY_INDEX;
    s_config.duration_min   = DEFAULT_DURATION_MIN;
    s_config.dim_lights     = DEFAULT_DIM_LIGHTS;
    s_config.dim_brightness = DEFAULT_DIM_BRIGHTNESS;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved feeding config – using defaults");
        return;
    }

    uint8_t u8;
    if (nvs_get_u8(h, NVS_KEY_RELAY, &u8)    == ESP_OK) s_config.relay_index    = (int)u8;
    if (nvs_get_u8(h, NVS_KEY_DURATION, &u8) == ESP_OK) s_config.duration_min   = (int)u8;
    if (nvs_get_u8(h, NVS_KEY_DIM, &u8)      == ESP_OK) s_config.dim_lights     = (u8 != 0);
    if (nvs_get_u8(h, NVS_KEY_DIM_BR, &u8)   == ESP_OK) s_config.dim_brightness = u8;

    nvs_close(h);
    ESP_LOGI(TAG, "Config: relay=%d dur=%dmin dim=%d dim_br=%d",
             s_config.relay_index, s_config.duration_min,
             s_config.dim_lights, s_config.dim_brightness);
}

static esp_err_t nvs_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, NVS_KEY_RELAY,    (uint8_t)s_config.relay_index);
    nvs_set_u8(h, NVS_KEY_DURATION, (uint8_t)s_config.duration_min);
    nvs_set_u8(h, NVS_KEY_DIM,      s_config.dim_lights ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_DIM_BR,   s_config.dim_brightness);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static void restore_state(void)
{
    /* Restore relay */
    if (s_config.relay_index >= 0 &&
        s_config.relay_index < RELAY_COUNT) {
        relay_controller_set(s_config.relay_index, s_relay_was_on);
        ESP_LOGI(TAG, "Relay %d restored to %s",
                 s_config.relay_index, s_relay_was_on ? "ON" : "OFF");
    }

    /* Restore LED brightness / state */
    if (s_config.dim_lights) {
        led_controller_set_brightness(s_prev_bright);
        if (!s_prev_led_on) {
            led_controller_cancel_fade();
            led_controller_off();
        }
        ESP_LOGI(TAG, "LED brightness restored to %d (was_on=%d)",
                 s_prev_bright, s_prev_led_on);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t feeding_mode_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();
    ESP_LOGI(TAG, "Feeding mode module initialised");
    return ESP_OK;
}

feeding_config_t feeding_mode_get_config(void)
{
    feeding_config_t cfg;
    if (s_mutex == NULL) {
        memset(&cfg, 0, sizeof(cfg));
        return cfg;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t feeding_mode_set_config(const feeding_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    feeding_config_t safe = *cfg;

    /* Clamp / validate */
    if (safe.relay_index < -1 || safe.relay_index >= RELAY_COUNT)
        safe.relay_index = -1;
    if (safe.duration_min < 1)  safe.duration_min = 1;
    if (safe.duration_min > 60) safe.duration_min = 60;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;
    xSemaphoreGive(s_mutex);

    return nvs_save_config();
}

esp_err_t feeding_mode_start(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    feeding_config_t cfg = s_config;

    /* Save current state before changing anything */
    if (cfg.relay_index >= 0 && cfg.relay_index < RELAY_COUNT) {
        s_relay_was_on = relay_controller_get(cfg.relay_index);
    }
    s_prev_bright = led_controller_get_brightness();
    s_prev_led_on = led_controller_is_on();

    /* Start timer */
    s_end_time = time(NULL) + (time_t)cfg.duration_min * 60;
    s_active   = true;
    xSemaphoreGive(s_mutex);

    /* Pause relay (turn OFF the filter/pump) */
    if (cfg.relay_index >= 0 && cfg.relay_index < RELAY_COUNT) {
        relay_controller_set(cfg.relay_index, false);
        ESP_LOGI(TAG, "Relay %d paused for feeding", cfg.relay_index);
    }

    /* Dim lights */
    if (cfg.dim_lights && led_controller_is_on()) {
        led_controller_set_brightness(cfg.dim_brightness);
        ESP_LOGI(TAG, "LED dimmed to %d for feeding", cfg.dim_brightness);
    }

    /* Telegram notification */
    {
        char msg[128];
        if (cfg.relay_index >= 0) {
            snprintf(msg, sizeof(msg),
                     "\xf0\x9f\x90\x9f <b>Modalit\xc3\xa0 Alimentazione ATTIVA</b>\n"
                     "Durata: %d minuti\n"
                     "Relay %d in pausa.",
                     cfg.duration_min, cfg.relay_index + 1);
        } else {
            snprintf(msg, sizeof(msg),
                     "\xf0\x9f\x90\x9f <b>Modalit\xc3\xa0 Alimentazione ATTIVA</b>\n"
                     "Durata: %d minuti",
                     cfg.duration_min);
        }
        telegram_notify_send(msg);
    }

    ESP_LOGI(TAG, "Feeding mode started (%d min)", cfg.duration_min);
    return ESP_OK;
}

void feeding_mode_stop(void)
{
    if (s_mutex == NULL) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool was_active = s_active;
    s_active   = false;
    s_end_time = 0;
    xSemaphoreGive(s_mutex);

    if (!was_active) return;

    restore_state();

    /* Telegram notification */
    telegram_notify_send(
        "\xf0\x9f\x90\x9f <b>Alimentazione terminata</b>\n"
        "Filtro e luci ripristinati.");

    ESP_LOGI(TAG, "Feeding mode stopped (manual or expired)");
}

bool feeding_mode_is_active(void)
{
    if (s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_active;
    xSemaphoreGive(s_mutex);
    return active;
}

int feeding_mode_get_remaining_s(void)
{
    if (s_mutex == NULL) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_active;
    time_t end  = s_end_time;
    xSemaphoreGive(s_mutex);

    if (!active) return 0;
    time_t now = time(NULL);
    int rem = (int)(end - now);
    return (rem > 0) ? rem : 0;
}

void feeding_mode_tick(void)
{
    if (s_mutex == NULL) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_active;
    time_t end  = s_end_time;
    xSemaphoreGive(s_mutex);

    if (!active) return;

    time_t now = time(NULL);
    if (now >= end) {
        /* Timer expired – restore state */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_active   = false;
        s_end_time = 0;
        xSemaphoreGive(s_mutex);

        restore_state();

        telegram_notify_send(
            "\xf0\x9f\x90\x9f <b>Alimentazione terminata</b>\n"
            "Filtro e luci ripristinati automaticamente.");

        ESP_LOGI(TAG, "Feeding mode expired – state restored");
    }
}
