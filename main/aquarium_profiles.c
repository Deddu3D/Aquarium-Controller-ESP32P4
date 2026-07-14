/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Aquarium Profiles implementation
 * Applies preset temperature, lighting, and CO2 settings for common setups.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"

#include "aquarium_profiles.h"
#include "auto_heater.h"
#include "relay_controller.h"
#include "co2_controller.h"
#include "telegram_notify.h"
#include "event_log.h"

static const char *TAG = "aq_profile";

static relay_schedule_t build_schedule(int on_h, int on_m, int off_h, int off_m)
{
    relay_schedule_t s = {0};
    s.enabled = true;
    s.on_min = (uint16_t)(on_h * 60 + on_m);
    s.off_min = (uint16_t)(off_h * 60 + off_m);
    return s;
}

esp_err_t aquarium_profile_apply(const char *type)
{
    if (type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *label = NULL;
    float target_temp = 25.0f;
    int on_h = 8, on_m = 0, off_h = 18, off_m = 0;
    bool co2_enabled = false;
    bool pause_enabled = false;
    int pause_start_h = 12, pause_start_m = 0, pause_end_h = 14, pause_end_m = 0;

    if (strcmp(type, "tropical") == 0) {
        label = "Tropicale";
        target_temp = 26.0f;
        on_h = 8; off_h = 18;
        co2_enabled = false;
    } else if (strcmp(type, "marine") == 0) {
        label = "Marino";
        target_temp = 25.0f;
        on_h = 8; off_h = 20;
        co2_enabled = false;
        pause_enabled = true;
    } else if (strcmp(type, "planted") == 0) {
        label = "Piantato";
        target_temp = 24.0f;
        on_h = 9; off_h = 17;
        co2_enabled = true;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    auto_heater_config_t heater = auto_heater_get_config();
    heater.target_temp_c = target_temp;

    co2_config_t co2 = co2_controller_get_config();
    co2.enabled = co2_enabled;

    esp_err_t err = auto_heater_set_config(&heater);
    if (err != ESP_OK) {
        return err;
    }
    if (pause_enabled) {
        relay_schedule_t pause1 = build_schedule(on_h, on_m, pause_start_h, pause_start_m);
        relay_schedule_t pause2 = build_schedule(pause_end_h, pause_end_m, off_h, off_m);
        err = relay_controller_set_schedule(0, 0, &pause1);
        if (err != ESP_OK) return err;
        err = relay_controller_set_schedule(0, 1, &pause2);
        if (err != ESP_OK) return err;
    } else {
        relay_schedule_t lights_main = build_schedule(on_h, on_m, off_h, off_m);
        err = relay_controller_set_schedule(0, 0, &lights_main);
        if (err != ESP_OK) return err;
    }

    relay_schedule_t disabled = {0};
    int first_unused_slot = pause_enabled ? 2 : 1;
    for (int slot = first_unused_slot; slot < RELAY_SCHEDULE_SLOTS; slot++) {
        err = relay_controller_set_schedule(0, slot, &disabled);
        if (err != ESP_OK) return err;
    }

    /* Force immediate re-evaluation from a known state so the updated
     * schedule applies deterministically right after profile activation. */
    relay_controller_set(0, false);
    relay_controller_tick_schedules();
    err = co2_controller_set_config(&co2);
    if (err != ESP_OK) {
        return err;
    }

    char notify[256];
    snprintf(notify, sizeof(notify),
             "\xF0\x9F\x90\xA0 <b>Profilo acquario applicato</b>\n"
             "Profilo: %s\n"
             "Temperatura target: %.1f\xC2\xB0" "C\n"
             "Luci (relè 1): %02d:%02d-%02d:%02d\n"
             "CO2: %s",
             label,
             (double)target_temp,
             on_h, on_m, off_h, off_m,
             co2_enabled ? "attiva" : "disattiva");
    telegram_notify_send(notify);

    char event_msg[EVENT_MSG_MAX];
    snprintf(event_msg, sizeof(event_msg), "Profilo acquario applicato: %s", label);
    event_log_add(EVT_SYSTEM, event_msg);
    ESP_LOGI(TAG, "Applied aquarium profile: %s", label);
    return ESP_OK;
}
