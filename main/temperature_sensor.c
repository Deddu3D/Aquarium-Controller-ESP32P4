/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - DS18B20 Water Temperature Sensor implementation
 * Periodically reads a DS18B20 on the configured 1-Wire bus and caches
 * the result for thread-safe retrieval.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 *
 * Design notes
 * ────────────
 * • Uses ds18b20_new_device_from_bus() (Skip ROM mode) – no ROM enumeration
 *   iterator.  A single sensor is the typical case and Skip ROM avoids the
 *   iterator's post-search RMT state that caused failures on ESP32-P4.
 * • Temperature conversion is triggered via ds18b20_trigger_temperature_conversion_for_all()
 *   then read back with ds18b20_get_temperature(), matching the pattern of
 *   the official espressif/ds18b20 example.
 * • No gpio_config() override after bus creation – the onewire_bus driver
 *   handles all GPIO setup internally (gpio_od_enable + pull mode).
 * • On any read failure the bus and device handles are destroyed and
 *   recreated on the next cycle, providing clean-slate recovery.
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "onewire_bus.h"
#include "ds18b20.h"

#include "temperature_sensor.h"
#include "telegram_notify.h"
#include "event_log.h"

static const char *TAG = "ds18b20";

/* Moving average window size – smooths out sensor noise */
#define TEMP_AVG_WINDOW  3

/* Consecutive failures before a Telegram alert is sent */
#define TEMP_FAULT_ALERT_THRESHOLD  5

/* ── Module state ────────────────────────────────────────────────── */

static onewire_bus_handle_t    s_bus    = NULL;
static ds18b20_device_handle_t s_device = NULL;

/* Cached reading – protected by mutex for safe cross-task access */
static SemaphoreHandle_t s_mutex      = NULL;
static float             s_temperature = 0.0f;
static bool              s_valid       = false;

/* Moving average ring buffer */
static float s_avg_buf[TEMP_AVG_WINDOW];
static int   s_avg_idx   = 0;
static int   s_avg_count = 0;

/* Fault alert tracking */
static int  s_fail_count = 0;
static bool s_alert_sent = false;

/* ── Bus / device lifecycle ──────────────────────────────────────── */

/**
 * @brief Release existing bus and device handles (safe to call with NULLs).
 */
static void destroy_bus_and_device(void)
{
    if (s_device) {
        ds18b20_del_device(s_device);
        s_device = NULL;
    }
    if (s_bus) {
        onewire_bus_del(s_bus);
        s_bus = NULL;
    }
}

/**
 * @brief Create the 1-Wire bus and a device handle (Skip ROM mode).
 *
 * Uses ds18b20_new_device_from_bus() so that a single sensor is addressed
 * via the Skip ROM command – no ROM enumeration iterator is needed.
 * The bus is created fresh, relying entirely on the onewire_bus driver for
 * GPIO setup (gpio_od_enable + floating pull mode when external pull-up
 * is present on the line).
 *
 * @return ESP_OK on success, error code otherwise.
 */
static esp_err_t create_bus_and_device(void)
{
    destroy_bus_and_device();

    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = CONFIG_DS18B20_GPIO,
        /* External 4.7 kΩ pull-up is present on the line; leave the weak
         * internal pull-up disabled so it does not interfere. */
        .flags = { .en_pull_up = 0 },
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        /* 1 byte ROM cmd + 8 byte address + 1 byte function cmd = 10 bytes */
        .max_rx_bytes = 10,
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "1-Wire bus creation failed on GPIO %d: %s",
                 CONFIG_DS18B20_GPIO, esp_err_to_name(err));
        return err;
    }

    ds18b20_config_t ds_cfg = {};
    err = ds18b20_new_device_from_bus(s_bus, &ds_cfg, &s_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DS18B20 device handle creation failed: %s",
                 esp_err_to_name(err));
        onewire_bus_del(s_bus);
        s_bus = NULL;
        return err;
    }

    ESP_LOGI(TAG, "1-Wire bus ready on GPIO %d (Skip ROM mode)", CONFIG_DS18B20_GPIO);
    return ESP_OK;
}

/* ── Reading task ────────────────────────────────────────────────── */

static void temperature_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_DS18B20_READ_INTERVAL_MS);

    while (1) {
        /* Recreate bus + device if they were torn down after a failure. */
        if (s_bus == NULL || s_device == NULL) {
            ESP_LOGW(TAG, "Recreating 1-Wire bus and device handle ...");
            if (create_bus_and_device() != ESP_OK) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_valid = false;
                xSemaphoreGive(s_mutex);
                vTaskDelay(interval);
                continue;
            }
        }

        /* Step 1 – Trigger temperature conversion for every sensor on the bus.
         * The driver waits the appropriate conversion time (800 ms for 12-bit)
         * before returning so ds18b20_get_temperature() can be called directly. */
        esp_err_t err = ds18b20_trigger_temperature_conversion_for_all(s_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Conversion trigger failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_valid = false;
            xSemaphoreGive(s_mutex);
            s_fail_count++;
            if (s_fail_count >= TEMP_FAULT_ALERT_THRESHOLD && !s_alert_sent) {
                s_alert_sent = true;
                telegram_notify_send(
                    "\xf0\x9f\x8c\xa1\xef\xb8\x8f <b>SENSORE TEMPERATURA GUASTO</b>\n"
                    "DS18B20: trigger conversione fallita ripetutamente.\n"
                    "Controllare il sensore e il cablaggio.");
                event_log_add(EVT_SENSOR_FAULT,
                              "DS18B20 conversion trigger failed repeatedly");
            }
            /* Destroy bus + device so the next iteration starts clean. */
            destroy_bus_and_device();
            vTaskDelay(interval);
            continue;
        }

        /* Step 2 – Read back the scratchpad. */
        float temp = 0.0f;
        err = ds18b20_get_temperature(s_device, &temp);
        if (err == ESP_OK) {
            /* Apply calibration offset from Kconfig */
            temp += ((float)CONFIG_DS18B20_CALIBRATION_OFFSET_CENTI) / 100.0f;

            /* Moving average – push new sample into ring buffer */
            s_avg_buf[s_avg_idx] = temp;
            s_avg_idx = (s_avg_idx + 1) % TEMP_AVG_WINDOW;
            if (s_avg_count < TEMP_AVG_WINDOW) {
                s_avg_count++;
            }

            /* Compute average of available samples */
            float sum = 0.0f;
            for (int i = 0; i < s_avg_count; i++) {
                sum += s_avg_buf[i];
            }
            float avg = sum / (float)s_avg_count;

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_temperature = avg;
            s_valid = true;
            xSemaphoreGive(s_mutex);

            /* Reset fault counters on success */
            s_fail_count = 0;
            s_alert_sent = false;

            ESP_LOGI(TAG, "Water temperature: %.2f °C (avg of %d)",
                     avg, s_avg_count);
        } else {
            ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_valid = false;
            xSemaphoreGive(s_mutex);
            s_fail_count++;
            if (s_fail_count >= TEMP_FAULT_ALERT_THRESHOLD && !s_alert_sent) {
                s_alert_sent = true;
                telegram_notify_send(
                    "\xf0\x9f\x8c\xa1\xef\xb8\x8f <b>SENSORE TEMPERATURA GUASTO</b>\n"
                    "DS18B20: lettura fallita ripetutamente.\n"
                    "Controllare il sensore e il cablaggio.");
                event_log_add(EVT_SENSOR_FAULT,
                              "DS18B20 read failed repeatedly");
            }
            /* Destroy bus + device so the next iteration starts clean. */
            destroy_bus_and_device();
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

    /* Create the 1-Wire bus and device handle.
     * The background task will keep retrying if the sensor is not
     * connected at boot time, so a failure here is non-fatal. */
    if (create_bus_and_device() != ESP_OK) {
        ESP_LOGW(TAG, "Initial bus/device setup failed – "
                 "background task will keep retrying");
    }

    /* Start the periodic reading task. */
    BaseType_t ret = xTaskCreate(temperature_task, "ds18b20",
                                 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create temperature task");
        destroy_bus_and_device();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool temperature_sensor_get(float *temp_c)
{
    if (temp_c == NULL || s_mutex == NULL) {
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
