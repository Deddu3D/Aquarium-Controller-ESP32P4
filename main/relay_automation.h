/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Relay Automation
 * Event-driven relay automation rules persisted in NVS.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_AUTO_MAX_RULES 8

typedef enum {
    RELAY_TRIG_TEMP_HIGH = 0,
    RELAY_TRIG_TEMP_LOW,
    RELAY_TRIG_LIGHTS_ON,
    RELAY_TRIG_FEEDING,
} relay_trig_t;

typedef struct {
    bool         enabled;
    relay_trig_t trigger;
    float        temp_threshold;
    int          relay_index;
    bool         action_on;
    int          duration_min;
} relay_auto_rule_t;

esp_err_t relay_auto_init(void);
void relay_auto_tick(void);
int relay_auto_get_rules(relay_auto_rule_t out[RELAY_AUTO_MAX_RULES]);
esp_err_t relay_auto_set_rules(const relay_auto_rule_t *rules, size_t count);

#ifdef __cplusplus
}
#endif
