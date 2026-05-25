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

#include "esp_log.h"

#include "aquarium_profiles.h"
#include "auto_heater.h"
#include "led_schedule.h"
#include "co2_controller.h"
#include "telegram_notify.h"
#include "event_log.h"

static const char *TAG = "aq_profile";

static void set_hhmm(uint8_t *hour, uint8_t *minute, int hh, int mm)
{
    *hour = (uint8_t)hh;
    *minute = (uint8_t)mm;
}

esp_err_t aquarium_profile_apply(const char *type)
{
    if (type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *label = NULL;
    float target_temp = 25.0f;
    int on_h = 8, on_m = 0, off_h = 18, off_m = 0;
    int ramp_min = 30;
    bool co2_enabled = false;
    bool pause_enabled = false;
    int pause_start_h = 12, pause_start_m = 0, pause_end_h = 14, pause_end_m = 0;

    if (strcmp(type, "tropical") == 0) {
        label = "Tropicale";
        target_temp = 26.0f;
        on_h = 8; off_h = 18;
        ramp_min = 30;
        co2_enabled = false;
    } else if (strcmp(type, "marine") == 0) {
        label = "Marino";
        target_temp = 25.0f;
        on_h = 8; off_h = 20;
        ramp_min = 30;
        co2_enabled = false;
        pause_enabled = true;
    } else if (strcmp(type, "planted") == 0) {
        label = "Piantato";
        target_temp = 24.0f;
        on_h = 9; off_h = 17;
        ramp_min = 20;
        co2_enabled = true;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    auto_heater_config_t heater = auto_heater_get_config();
    heater.target_temp_c = target_temp;

    led_schedule_config_t led = led_schedule_get_config();
    led.enabled = true;
    set_hhmm(&led.on_hour, &led.on_minute, on_h, on_m);
    set_hhmm(&led.off_hour, &led.off_minute, off_h, off_m);
    led.ramp_duration_min = (uint16_t)ramp_min;
    led.pause_enabled = pause_enabled;
    if (pause_enabled) {
        set_hhmm(&led.pause_start_hour, &led.pause_start_minute, pause_start_h, pause_start_m);
        set_hhmm(&led.pause_end_hour, &led.pause_end_minute, pause_end_h, pause_end_m);
        led.pause_brightness = (uint8_t)((led.brightness > 0) ? ((led.brightness * 20) / 100) : 20);
        if (led.pause_brightness == 0) {
            led.pause_brightness = 20;
        }
        led.pause_red = (uint8_t)((led.red * 20) / 100);
        led.pause_green = (uint8_t)((led.green * 20) / 100);
        led.pause_blue = (uint8_t)((led.blue * 20) / 100);
    }

    co2_config_t co2 = co2_controller_get_config();
    co2.enabled = co2_enabled;

    esp_err_t err = auto_heater_set_config(&heater);
    if (err != ESP_OK) {
        return err;
    }
    err = led_schedule_set_config(&led);
    if (err != ESP_OK) {
        return err;
    }
    err = co2_controller_set_config(&co2);
    if (err != ESP_OK) {
        return err;
    }

    char notify[256];
    snprintf(notify, sizeof(notify),
             "\xF0\x9F\x90\xA0 <b>Profilo acquario applicato</b>\n"
             "Profilo: %s\n"
             "Temperatura target: %.1f\xC2\xB0C\n"
             "Luci: %02d:%02d-%02d:%02d\n"
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
