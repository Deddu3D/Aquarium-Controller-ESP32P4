/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - DS18B20 Water Temperature Sensor implementation
 * Periodically reads a DS18B20 on the configured 1-Wire bus and caches
 * the result for thread-safe retrieval.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "onewire_bus.h"
#include "ds18b20.h"

#include "temperature_sensor.h"

static const char *TAG = "ds18b20";

/* Maximum number of DS18B20 devices we support on one bus */
#define MAX_DS18B20  4

static ds18b20_device_handle_t s_devices[MAX_DS18B20];
static int                     s_device_count;
static onewire_bus_handle_t    s_bus;

/* Cached reading – protected by mutex for safe cross-task access */
static SemaphoreHandle_t s_mutex;
static float s_temperature;
static bool  s_valid;

/* ── Reading task ────────────────────────────────────────────────── */

static void temperature_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_DS18B20_READ_INTERVAL_MS);

    while (1) {
        /* Trigger conversion on all devices sharing the bus */
        esp_err_t err = ds18b20_trigger_temperature_conversion(s_devices[0]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Conversion trigger failed: %s", esp_err_to_name(err));
            s_valid = false;
            vTaskDelay(interval);
            continue;
        }

        /* Read the first sensor (primary water probe) */
        float temp = 0.0f;
        err = ds18b20_get_temperature(s_devices[0], &temp);
        if (err == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_temperature = temp;
            s_valid = true;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Water temperature: %.2f °C", temp);
        } else {
            ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_valid = false;
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(interval);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t temperature_sensor_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* 1. Create the 1-Wire bus using the RMT peripheral */
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = CONFIG_DS18B20_GPIO,
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,   /* ROM cmd + 8-byte address + device cmd */
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 1-Wire bus on GPIO %d: %s",
                 CONFIG_DS18B20_GPIO, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "1-Wire bus created on GPIO %d", CONFIG_DS18B20_GPIO);

    /* 2. Enumerate all DS18B20 devices */
    onewire_device_iter_handle_t iter = NULL;
    err = onewire_new_device_iter(s_bus, &iter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator: %s", esp_err_to_name(err));
        return err;
    }

    s_device_count = 0;
    onewire_device_t next_dev;
    while (s_device_count < MAX_DS18B20) {
        err = onewire_device_iter_get_next(iter, &next_dev);
        if (err != ESP_OK) {
            break;   /* ESP_ERR_NOT_FOUND = no more devices */
        }
        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(&next_dev, &ds_cfg,
                               &s_devices[s_device_count]) == ESP_OK) {
            ESP_LOGI(TAG, "Found DS18B20 #%d", s_device_count);
            s_device_count++;
        }
    }
    onewire_del_device_iter(iter);

    if (s_device_count == 0) {
        ESP_LOGE(TAG, "No DS18B20 sensors found on GPIO %d", CONFIG_DS18B20_GPIO);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Total DS18B20 devices: %d", s_device_count);

    /* 3. Start periodic reading task */
    BaseType_t ret = xTaskCreate(temperature_task, "ds18b20",
                                 3072, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create temperature task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool temperature_sensor_get(float *temp_c)
{
    if (temp_c == NULL) {
        return false;
    }
    bool valid;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    valid = s_valid;
    if (valid) {
        *temp_c = s_temperature;
    }
    xSemaphoreGive(s_mutex);
    return valid;
}
