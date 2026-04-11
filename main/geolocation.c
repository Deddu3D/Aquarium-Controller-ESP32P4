/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Geolocation Configuration implementation
 * NVS-backed lat/lng/UTC offset with thread-safe access.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "geolocation.h"

static const char *TAG = "geoloc";

/* NVS namespace and keys */
#define NVS_NAMESPACE   "geoloc"
#define NVS_KEY_LAT     "lat"
#define NVS_KEY_LNG     "lng"
#define NVS_KEY_UTC_OFF "utc_off"

/* Default: Rome, Italy  41.9028°N  12.4964°E  UTC+1 (60 min) */
#define DEFAULT_LAT       41.9028
#define DEFAULT_LNG       12.4964
#define DEFAULT_UTC_OFF   60

static SemaphoreHandle_t     s_mutex = NULL;
static geolocation_config_t  s_config;

/* ── NVS helpers ─────────────────────────────────────────────────── */

/**
 * @brief Store a double as two 32-bit words in NVS.
 */
static esp_err_t nvs_set_double(nvs_handle_t h, const char *key, double val)
{
    uint64_t raw;
    memcpy(&raw, &val, sizeof(raw));

    char key_lo[16], key_hi[16];
    snprintf(key_lo, sizeof(key_lo), "%s_lo", key);
    snprintf(key_hi, sizeof(key_hi), "%s_hi", key);

    esp_err_t err = nvs_set_u32(h, key_lo, (uint32_t)(raw & 0xFFFFFFFF));
    if (err != ESP_OK) return err;
    return nvs_set_u32(h, key_hi, (uint32_t)(raw >> 32));
}

/**
 * @brief Read a double stored as two 32-bit words from NVS.
 */
static esp_err_t nvs_get_double(nvs_handle_t h, const char *key, double *val)
{
    char key_lo[16], key_hi[16];
    snprintf(key_lo, sizeof(key_lo), "%s_lo", key);
    snprintf(key_hi, sizeof(key_hi), "%s_hi", key);

    uint32_t lo = 0, hi = 0;
    esp_err_t err = nvs_get_u32(h, key_lo, &lo);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(h, key_hi, &hi);
    if (err != ESP_OK) return err;

    uint64_t raw = ((uint64_t)hi << 32) | lo;
    memcpy(val, &raw, sizeof(*val));
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t geolocation_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Start with defaults */
    s_config.latitude       = DEFAULT_LAT;
    s_config.longitude      = DEFAULT_LNG;
    s_config.utc_offset_min = DEFAULT_UTC_OFF;

    /* Try to load from NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_get_double(h, NVS_KEY_LAT, &s_config.latitude);
        nvs_get_double(h, NVS_KEY_LNG, &s_config.longitude);

        int32_t off = 0;
        if (nvs_get_i32(h, NVS_KEY_UTC_OFF, &off) == ESP_OK) {
            s_config.utc_offset_min = (int)off;
        }
        nvs_close(h);
        ESP_LOGI(TAG, "Loaded from NVS: lat=%.4f lng=%.4f utc_off=%d",
                 s_config.latitude, s_config.longitude, s_config.utc_offset_min);
    } else {
        ESP_LOGI(TAG, "No saved config – using defaults (Rome, Italy)");
    }

    return ESP_OK;
}

geolocation_config_t geolocation_get(void)
{
    geolocation_config_t cfg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t geolocation_set(const geolocation_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Clamp latitude/longitude to valid ranges */
    geolocation_config_t safe = *cfg;
    if (safe.latitude < -90.0)  safe.latitude = -90.0;
    if (safe.latitude >  90.0)  safe.latitude =  90.0;
    if (safe.longitude < -180.0) safe.longitude = -180.0;
    if (safe.longitude >  180.0) safe.longitude =  180.0;
    if (safe.utc_offset_min < -720) safe.utc_offset_min = -720;
    if (safe.utc_offset_min >  840) safe.utc_offset_min =  840;

    /* Persist to NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_double(h, NVS_KEY_LAT, safe.latitude);
    nvs_set_double(h, NVS_KEY_LNG, safe.longitude);
    nvs_set_i32(h, NVS_KEY_UTC_OFF, (int32_t)safe.utc_offset_min);
    nvs_commit(h);
    nvs_close(h);

    /* Update in-memory copy */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Updated: lat=%.4f lng=%.4f utc_off=%d",
             safe.latitude, safe.longitude, safe.utc_offset_min);
    return ESP_OK;
}
