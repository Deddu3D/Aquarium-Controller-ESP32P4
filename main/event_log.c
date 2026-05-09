/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Event Log implementation
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "event_log.h"

static const char *TAG = "event_log";

static event_entry_t     s_ring[EVENT_LOG_MAX];
static int               s_head  = 0;   /* next write position */
static int               s_count = 0;   /* valid entries       */
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t event_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    memset(s_ring, 0, sizeof(s_ring));
    s_head  = 0;
    s_count = 0;
    ESP_LOGI(TAG, "Event log initialised (%d slots)", EVENT_LOG_MAX);
    return ESP_OK;
}

void event_log_add(event_type_t type, const char *message)
{
    if (s_mutex == NULL || message == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_ring[s_head].timestamp = time(NULL);
    s_ring[s_head].type      = type;
    strncpy(s_ring[s_head].message, message, EVENT_MSG_MAX - 1);
    s_ring[s_head].message[EVENT_MSG_MAX - 1] = '\0';

    s_head = (s_head + 1) % EVENT_LOG_MAX;
    if (s_count < EVENT_LOG_MAX) {
        s_count++;
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Event [%d]: %s", (int)type, message);
}

void event_log_get_all(event_entry_t *out, int *out_count)
{
    if (out == NULL || out_count == NULL) {
        return;
    }
    if (s_mutex == NULL) {
        *out_count = 0;
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Oldest entry is at (s_head - s_count) mod size */
    int start = (s_head - s_count + EVENT_LOG_MAX) % EVENT_LOG_MAX;
    for (int i = 0; i < s_count; i++) {
        out[i] = s_ring[(start + i) % EVENT_LOG_MAX];
    }
    *out_count = s_count;

    xSemaphoreGive(s_mutex);
}
