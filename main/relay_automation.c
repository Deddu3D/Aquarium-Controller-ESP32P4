/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Relay Automation implementation
 * Event-driven relay rules based on temperature, lighting, and feeding state.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "relay_automation.h"
#include "relay_controller.h"
#include "temperature_sensor.h"
#include "led_controller.h"
#include "feeding_mode.h"

static const char *TAG = "relay_auto";

#define NVS_NAMESPACE "relay_auto"
#define NVS_KEY_RULES "rules"

static SemaphoreHandle_t s_mutex = NULL;
static relay_auto_rule_t s_rules[RELAY_AUTO_MAX_RULES];
static bool s_prev_condition[RELAY_AUTO_MAX_RULES];
static bool s_restore_pending[RELAY_AUTO_MAX_RULES];
static bool s_restore_state[RELAY_AUTO_MAX_RULES];
static time_t s_restore_deadline[RELAY_AUTO_MAX_RULES];

static relay_auto_rule_t sanitize_rule(relay_auto_rule_t rule)
{
    if (rule.trigger < RELAY_TRIG_TEMP_HIGH || rule.trigger > RELAY_TRIG_FEEDING) {
        rule.trigger = RELAY_TRIG_TEMP_HIGH;
    }
    if (rule.relay_index < 0 || rule.relay_index >= RELAY_COUNT) {
        rule.relay_index = 0;
    }
    if (rule.temp_threshold < 0.0f) {
        rule.temp_threshold = 0.0f;
    }
    if (rule.temp_threshold > 50.0f) {
        rule.temp_threshold = 50.0f;
    }
    if (rule.duration_min < 0) {
        rule.duration_min = 0;
    }
    if (rule.duration_min > 1440) {
        rule.duration_min = 1440;
    }
    return rule;
}

static void reset_runtime_state(void)
{
    memset(s_prev_condition, 0, sizeof(s_prev_condition));
    memset(s_restore_pending, 0, sizeof(s_restore_pending));
    memset(s_restore_state, 0, sizeof(s_restore_state));
    memset(s_restore_deadline, 0, sizeof(s_restore_deadline));
}

static void nvs_load_rules(void)
{
    memset(s_rules, 0, sizeof(s_rules));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved relay automation rules – using defaults");
        return;
    }

    size_t len = sizeof(s_rules);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_RULES, s_rules, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_rules)) {
        memset(s_rules, 0, sizeof(s_rules));
        ESP_LOGW(TAG, "Invalid relay automation rules in NVS – reset to defaults");
        return;
    }

    for (int i = 0; i < RELAY_AUTO_MAX_RULES; i++) {
        s_rules[i] = sanitize_rule(s_rules[i]);
    }
}

static esp_err_t nvs_save_rules(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY_RULES, s_rules, sizeof(s_rules));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool evaluate_condition(const relay_auto_rule_t *rule,
                               bool temp_valid,
                               float temp_c,
                               bool lights_on,
                               bool feeding_active)
{
    switch (rule->trigger) {
    case RELAY_TRIG_TEMP_HIGH:
        return temp_valid && temp_c > rule->temp_threshold;
    case RELAY_TRIG_TEMP_LOW:
        return temp_valid && temp_c < rule->temp_threshold;
    case RELAY_TRIG_LIGHTS_ON:
        return lights_on;
    case RELAY_TRIG_FEEDING:
        return feeding_active;
    default:
        return false;
    }
}

esp_err_t relay_auto_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_rules();
    reset_runtime_state();
    ESP_LOGI(TAG, "Relay automation module initialised");
    return ESP_OK;
}

int relay_auto_get_rules(relay_auto_rule_t out[RELAY_AUTO_MAX_RULES])
{
    if (s_mutex == NULL || out == NULL) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, s_rules, sizeof(s_rules));
    xSemaphoreGive(s_mutex);
    return RELAY_AUTO_MAX_RULES;
}

esp_err_t relay_auto_set_rules(const relay_auto_rule_t *rules, size_t count)
{
    if (s_mutex == NULL || (count > 0 && rules == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    relay_auto_rule_t safe[RELAY_AUTO_MAX_RULES];
    memset(safe, 0, sizeof(safe));
    if (count > RELAY_AUTO_MAX_RULES) {
        count = RELAY_AUTO_MAX_RULES;
    }
    for (size_t i = 0; i < count; i++) {
        safe[i] = sanitize_rule(rules[i]);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_rules, safe, sizeof(s_rules));
    reset_runtime_state();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Relay automation rules updated (%u active slots)", (unsigned)count);
    return nvs_save_rules();
}

void relay_auto_tick(void)
{
    if (s_mutex == NULL) {
        return;
    }

    relay_auto_rule_t rules[RELAY_AUTO_MAX_RULES];
    bool prev_condition[RELAY_AUTO_MAX_RULES];
    bool restore_pending[RELAY_AUTO_MAX_RULES];
    bool restore_state[RELAY_AUTO_MAX_RULES];
    time_t restore_deadline[RELAY_AUTO_MAX_RULES];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(rules, s_rules, sizeof(rules));
    memcpy(prev_condition, s_prev_condition, sizeof(prev_condition));
    memcpy(restore_pending, s_restore_pending, sizeof(restore_pending));
    memcpy(restore_state, s_restore_state, sizeof(restore_state));
    memcpy(restore_deadline, s_restore_deadline, sizeof(restore_deadline));
    xSemaphoreGive(s_mutex);

    time_t now = time(NULL);
    for (int i = 0; i < RELAY_AUTO_MAX_RULES; i++) {
        if (!restore_pending[i] || now < restore_deadline[i]) {
            continue;
        }
        relay_controller_set(rules[i].relay_index, restore_state[i]);
        restore_pending[i] = false;
        ESP_LOGI(TAG, "Rule %d expired – relay %d restored to %s",
                 i, rules[i].relay_index, restore_state[i] ? "ON" : "OFF");
    }

    float temp_c = 0.0f;
    bool temp_valid = temperature_sensor_get(&temp_c);
    bool lights_on = led_controller_is_on();
    bool feeding_active = feeding_mode_is_active();

    for (int i = 0; i < RELAY_AUTO_MAX_RULES; i++) {
        if (!rules[i].enabled) {
            prev_condition[i] = false;
            continue;
        }

        bool condition = evaluate_condition(&rules[i], temp_valid, temp_c, lights_on, feeding_active);
        if (condition && !prev_condition[i]) {
            if (rules[i].duration_min > 0) {
                restore_state[i] = relay_controller_get(rules[i].relay_index);
                restore_deadline[i] = now + (time_t)rules[i].duration_min * 60;
                restore_pending[i] = true;
            }
            relay_controller_set(rules[i].relay_index, rules[i].action_on);
            ESP_LOGI(TAG, "Rule %d triggered – relay %d %s%s",
                     i,
                     rules[i].relay_index,
                     rules[i].action_on ? "ON" : "OFF",
                     rules[i].duration_min > 0 ? " (timed)" : "");
        }
        prev_condition[i] = condition;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_prev_condition, prev_condition, sizeof(s_prev_condition));
    memcpy(s_restore_pending, restore_pending, sizeof(s_restore_pending));
    memcpy(s_restore_state, restore_state, sizeof(s_restore_state));
    memcpy(s_restore_deadline, restore_deadline, sizeof(s_restore_deadline));
    xSemaphoreGive(s_mutex);
}
