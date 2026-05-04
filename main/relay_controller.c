/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - 4-Channel Relay Controller implementation
 * Drives four GPIOs (active-high by default) for 3 V relay modules
 * and persists state + custom names in NVS.
 * Each relay now supports RELAY_SCHEDULE_SLOTS independent time slots.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "relay_controller.h"
#include "sd_logger.h"

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

/* Runtime state – protected by s_mutex for thread safety */
static relay_state_t     s_relay[RELAY_COUNT];
static SemaphoreHandle_t s_mutex = NULL;
static relay_change_cb_t s_change_cb = NULL;

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
            memset(s_relay[i].schedules, 0,
                   sizeof(s_relay[i].schedules));
        }
        return;
    }

    for (int i = 0; i < RELAY_COUNT; i++) {
        char key[16];

        /* Load on/off state */
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

        /* Load schedule slots (stored as a blob array) */
        memset(s_relay[i].schedules, 0, sizeof(s_relay[i].schedules));
        snprintf(key, sizeof(key), "sched%d", i);
        size_t sched_len = sizeof(s_relay[i].schedules);
        nvs_get_blob(h, key, &s_relay[i].schedules, &sched_len);
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
    char key[16];
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
    char key[16];
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

static void nvs_save_schedules(int index)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed for schedule save");
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), "sched%d", index);
    nvs_set_blob(h, key, s_relay[index].schedules,
                 sizeof(s_relay[index].schedules));
    nvs_commit(h);
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
    /* Create mutex for thread-safe state access */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create relay mutex");
        return ESP_ERR_NO_MEM;
    }

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

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool prev = s_relay[index].on;
    s_relay[index].on = on;
    gpio_apply(index);
    relay_change_cb_t cb = s_change_cb;
    xSemaphoreGive(s_mutex);

    nvs_save_state(index);
    ESP_LOGI(TAG, "Relay %d (%s) → %s",
             index, s_relay[index].name, on ? "ON" : "OFF");

    /* Fire change callback only when state actually changes */
    if (cb && prev != on) {
        cb(index, on, "manual");
    }

    /* Log relay event to SD card */
    if (prev != on) {
        char detail[48];
        snprintf(detail, sizeof(detail), "relay%d(%s)", index, s_relay[index].name);
        sd_logger_log_event(time(NULL), on ? "relay_on" : "relay_off", detail);
    }

    return ESP_OK;
}

bool relay_controller_get(int index)
{
    if (index < 0 || index >= RELAY_COUNT) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool on = s_relay[index].on;
    xSemaphoreGive(s_mutex);
    return on;
}

esp_err_t relay_controller_set_name(int index, const char *name)
{
    if (index < 0 || index >= RELAY_COUNT || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_relay[index].name, name, RELAY_NAME_MAX - 1);
    s_relay[index].name[RELAY_NAME_MAX - 1] = '\0';
    xSemaphoreGive(s_mutex);
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(name, s_relay[index].name, len - 1);
    name[len - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

void relay_controller_get_all(relay_state_t out[RELAY_COUNT])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, s_relay, sizeof(s_relay));
    xSemaphoreGive(s_mutex);
}

esp_err_t relay_controller_set_schedule(int index, int slot,
                                        const relay_schedule_t *schedule)
{
    if (index < 0 || index >= RELAY_COUNT ||
        slot  < 0 || slot  >= RELAY_SCHEDULE_SLOTS ||
        schedule == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    relay_schedule_t safe = *schedule;
    if (safe.on_min  > 1439) safe.on_min  = 1439;
    if (safe.off_min > 1439) safe.off_min = 1439;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_relay[index].schedules[slot] = safe;
    xSemaphoreGive(s_mutex);

    nvs_save_schedules(index);
    ESP_LOGI(TAG, "Relay %d slot %d: enabled=%d on=%02d:%02d off=%02d:%02d",
             index, slot, safe.enabled,
             safe.on_min / 60, safe.on_min % 60,
             safe.off_min / 60, safe.off_min % 60);
    return ESP_OK;
}

esp_err_t relay_controller_set_all_schedules(int index,
                                             const relay_schedule_t schedules[RELAY_SCHEDULE_SLOTS])
{
    if (index < 0 || index >= RELAY_COUNT || schedules == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int s = 0; s < RELAY_SCHEDULE_SLOTS; s++) {
        s_relay[index].schedules[s] = schedules[s];
        if (s_relay[index].schedules[s].on_min  > 1439)
            s_relay[index].schedules[s].on_min  = 1439;
        if (s_relay[index].schedules[s].off_min > 1439)
            s_relay[index].schedules[s].off_min = 1439;
    }
    xSemaphoreGive(s_mutex);
    nvs_save_schedules(index);
    return ESP_OK;
}

void relay_controller_tick_schedules(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);

    /* Skip if clock not synced */
    if (ti.tm_year < (2024 - 1900)) {
        return;
    }

    int now_min = ti.tm_hour * 60 + ti.tm_min;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < RELAY_COUNT; i++) {
        /* Compute logical OR of all active slots */
        bool should_be_on = false;
        for (int s = 0; s < RELAY_SCHEDULE_SLOTS; s++) {
            if (!s_relay[i].schedules[s].enabled) continue;

            uint16_t on_m  = s_relay[i].schedules[s].on_min;
            uint16_t off_m = s_relay[i].schedules[s].off_min;

            bool in_window;
            if (on_m <= off_m) {
                in_window = (now_min >= (int)on_m && now_min < (int)off_m);
            } else {
                in_window = (now_min >= (int)on_m || now_min < (int)off_m);
            }
            if (in_window) {
                should_be_on = true;
                break;
            }
        }

        if (should_be_on != s_relay[i].on) {
            s_relay[i].on = should_be_on;
            gpio_apply(i);
            relay_change_cb_t cb = s_change_cb;
            /* Release the lock before invoking the callback to prevent
             * deadlocks if the callback calls back into this module.
             * There is a brief window where relay state is modified but
             * the callback has not yet fired; this is intentional and
             * acceptable for a single-threaded main-loop design. */
            xSemaphoreGive(s_mutex);

            ESP_LOGI(TAG, "Schedule: Relay %d (%s) → %s",
                     i, s_relay[i].name, should_be_on ? "ON" : "OFF");

            if (cb) {
                cb(i, should_be_on, "schedule");
            }

            /* Log relay schedule event to SD card */
            {
                /* "relay%d(%s) src=schedule": 5 + 2 + RELAY_NAME_MAX + 15 + NUL */
                char detail[8 + RELAY_NAME_MAX];
                snprintf(detail, sizeof(detail), "relay%d(%s) src=schedule",
                         i, s_relay[i].name);
                sd_logger_log_event(now, should_be_on ? "relay_on" : "relay_off",
                                    detail);
            }

            xSemaphoreTake(s_mutex, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_mutex);
}

void relay_controller_set_change_cb(relay_change_cb_t cb)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_change_cb = cb;
    xSemaphoreGive(s_mutex);
}
