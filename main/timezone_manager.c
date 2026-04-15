/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Timezone Manager implementation
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "timezone_manager.h"

static const char *TAG = "tz_mgr";

#define NVS_NAMESPACE "timezone"
#define NVS_KEY_TZ    "tz"

/* Kconfig fallback default */
#ifndef CONFIG_AQUARIUM_DEFAULT_TZ
#define CONFIG_AQUARIUM_DEFAULT_TZ "CET-1CEST,M3.5.0/2,M10.5.0/3"
#endif

static char s_tz[TZ_STRING_MAX] = CONFIG_AQUARIUM_DEFAULT_TZ;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void apply_tz(void)
{
    setenv("TZ", s_tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone applied: %s", s_tz);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t timezone_manager_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_tz);
        if (nvs_get_str(h, NVS_KEY_TZ, s_tz, &len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded TZ from NVS: %s", s_tz);
        } else {
            ESP_LOGI(TAG, "No TZ in NVS – using default: %s", s_tz);
        }
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "NVS namespace not found – using default: %s", s_tz);
    }

    apply_tz();
    return ESP_OK;
}

void timezone_manager_get(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;
    strncpy(buf, s_tz, len - 1);
    buf[len - 1] = '\0';
}

esp_err_t timezone_manager_set(const char *tz)
{
    if (tz == NULL || tz[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(tz) >= TZ_STRING_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_tz, tz, sizeof(s_tz) - 1);
    s_tz[sizeof(s_tz) - 1] = '\0';
    apply_tz();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_set_str(h, NVS_KEY_TZ, s_tz);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Timezone saved: %s", s_tz);
    return ESP_OK;
}
