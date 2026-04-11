/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - 4-Channel Relay Controller implementation
 * Drives four GPIOs (active-high by default) for 3 V relay modules
 * and persists state + custom names in NVS.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "relay_controller.h"

static const char *TAG = "relay";

/* NVS namespace for relay state persistence */
#define NVS_NAMESPACE "relays"

/* GPIO pins for each relay – configured in Kconfig */
static const gpio_num_t s_relay_gpio[RELAY_COUNT] = {
    CONFIG_RELAY_1_GPIO,
    CONFIG_RELAY_2_GPIO,
    CONFIG_RELAY_3_GPIO,
    CONFIG_RELAY_4_GPIO,
};

/* Default names for each channel */
static const char *s_default_names[RELAY_COUNT] = {
    "Relay 1",
    "Relay 2",
    "Relay 3",
    "Relay 4",
};

/* Runtime state */
static relay_state_t s_relay[RELAY_COUNT];

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        /* First boot – use defaults */
        for (int i = 0; i < RELAY_COUNT; i++) {
            s_relay[i].on = false;
            strncpy(s_relay[i].name, s_default_names[i],
                    RELAY_NAME_MAX - 1);
            s_relay[i].name[RELAY_NAME_MAX - 1] = '\0';
        }
        return;
    }

    for (int i = 0; i < RELAY_COUNT; i++) {
        /* Load on/off state */
        char key[12];
        snprintf(key, sizeof(key), "on%d", i);
        uint8_t val = 0;
        if (nvs_get_u8(h, key, &val) == ESP_OK) {
            s_relay[i].on = (val != 0);
        } else {
            s_relay[i].on = false;
        }

        /* Load custom name */
        snprintf(key, sizeof(key), "name%d", i);
        size_t len = RELAY_NAME_MAX;
        if (nvs_get_str(h, key, s_relay[i].name, &len) != ESP_OK) {
            strncpy(s_relay[i].name, s_default_names[i],
                    RELAY_NAME_MAX - 1);
            s_relay[i].name[RELAY_NAME_MAX - 1] = '\0';
        }
    }

    nvs_close(h);
}

static void nvs_save_state(int index)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed for relay state save");
        return;
    }
    char key[12];
    snprintf(key, sizeof(key), "on%d", index);
    esp_err_t err = nvs_set_u8(h, key, s_relay[index].on ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(err));
    }
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

static void nvs_save_name(int index)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed for relay name save");
        return;
    }
    char key[12];
    snprintf(key, sizeof(key), "name%d", index);
    esp_err_t err = nvs_set_str(h, key, s_relay[index].name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

/* ── GPIO helpers ────────────────────────────────────────────────── */

static void gpio_apply(int index)
{
#if CONFIG_RELAY_ACTIVE_LOW
    gpio_set_level(s_relay_gpio[index], s_relay[index].on ? 0 : 1);
#else
    gpio_set_level(s_relay_gpio[index], s_relay[index].on ? 1 : 0);
#endif
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t relay_controller_init(void)
{
    /* Load persisted state and names from NVS */
    nvs_load();

    /* Configure each GPIO as push-pull output */
    for (int i = 0; i < RELAY_COUNT; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_relay_gpio[i]),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO %d config failed: %s",
                     s_relay_gpio[i], esp_err_to_name(err));
            return err;
        }

        /* Apply the restored state */
        gpio_apply(i);
        ESP_LOGI(TAG, "Relay %d (%s) on GPIO %d – %s",
                 i, s_relay[i].name, s_relay_gpio[i],
                 s_relay[i].on ? "ON" : "OFF");
    }

    return ESP_OK;
}

esp_err_t relay_controller_set(int index, bool on)
{
    if (index < 0 || index >= RELAY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_relay[index].on = on;
    gpio_apply(index);
    nvs_save_state(index);
    ESP_LOGI(TAG, "Relay %d (%s) → %s",
             index, s_relay[index].name, on ? "ON" : "OFF");
    return ESP_OK;
}

bool relay_controller_get(int index)
{
    if (index < 0 || index >= RELAY_COUNT) {
        return false;
    }
    return s_relay[index].on;
}

esp_err_t relay_controller_set_name(int index, const char *name)
{
    if (index < 0 || index >= RELAY_COUNT || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_relay[index].name, name, RELAY_NAME_MAX - 1);
    s_relay[index].name[RELAY_NAME_MAX - 1] = '\0';
    nvs_save_name(index);
    ESP_LOGI(TAG, "Relay %d renamed to \"%s\"", index, s_relay[index].name);
    return ESP_OK;
}

void relay_controller_get_name(int index, char *name, size_t len)
{
    if (index < 0 || index >= RELAY_COUNT || name == NULL || len == 0) {
        if (name && len > 0) {
            name[0] = '\0';
        }
        return;
    }
    strncpy(name, s_relay[index].name, len - 1);
    name[len - 1] = '\0';
}

void relay_controller_get_all(relay_state_t out[RELAY_COUNT])
{
    memcpy(out, s_relay, sizeof(s_relay));
}
