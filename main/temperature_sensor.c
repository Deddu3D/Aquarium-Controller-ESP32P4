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
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "driver/gpio.h"
#include "onewire_bus.h"
#include "ds18b20.h"

#include "temperature_sensor.h"
#include "telegram_notify.h"
#include "event_log.h"

static const char *TAG = "ds18b20";

/* Maximum number of DS18B20 devices we support on one bus */
#define MAX_DS18B20  4

/* Moving average window size – smooths out sensor noise */
#define TEMP_AVG_WINDOW  3

/* Consecutive failures before a Telegram alert is sent */
#define TEMP_FAULT_ALERT_THRESHOLD  5

/* Retry parameters – sensor may not be ready immediately after power-on or
 * soft reset: give it up to SENSOR_DETECT_RETRIES attempts, each separated
 * by SENSOR_DETECT_DELAY_MS milliseconds. */
#define SENSOR_DETECT_RETRIES   3
#define SENSOR_DETECT_DELAY_MS  500

static ds18b20_device_handle_t s_devices[MAX_DS18B20];
static int                     s_device_count;
static onewire_bus_handle_t    s_bus;

/* Cached reading – protected by mutex for safe cross-task access */
static SemaphoreHandle_t s_mutex;
static float s_temperature;
static bool  s_valid;

/* Moving average ring buffer */
static float s_avg_buf[TEMP_AVG_WINDOW];
static int   s_avg_idx;
static int   s_avg_count;

/* Fault alert tracking */
static int  s_fail_count   = 0;
static bool s_alert_sent   = false;

/* ── Bus lifecycle helpers ───────────────────────────────────────── */

/**
 * @brief Delete the current 1-Wire bus and allocate a fresh one.
 *
 * After any RMT receive timeout the RX channel is left in a disabled state
 * and subsequent calls fail with "channel not in enable state".  The only
 * reliable recovery is to delete the bus handle and create a new one, which
 * resets the RMT channel back to the idle/enabled state.
 *
 * @return ESP_OK on success, error code otherwise.
 */
static esp_err_t recreate_bus(void)
{
    if (s_bus) {
        onewire_bus_del(s_bus);
        s_bus = NULL;
    }

    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = CONFIG_DS18B20_GPIO,
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,   /* ROM cmd + 8-byte address + device cmd */
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to (re)create 1-Wire bus on GPIO %d: %s",
                 CONFIG_DS18B20_GPIO, esp_err_to_name(err));
    }
    return err;
}

/* ── Device-scan helper ──────────────────────────────────────────── */

/**
 * @brief Scan the 1-Wire bus and populate s_devices / s_device_count.
 *
 * May be called from both temperature_sensor_init() (with retry) and from
 * temperature_task() when a sensor reconnect is needed.
 *
 * @return Number of DS18B20 devices found (≥ 0).
 */
static int scan_devices(void)
{
    s_device_count = 0;

    onewire_device_iter_handle_t iter = NULL;
    esp_err_t err = onewire_new_device_iter(s_bus, &iter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Device iterator creation failed: %s", esp_err_to_name(err));
        return 0;
    }

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

    return s_device_count;
}

/* ── Reading task ────────────────────────────────────────────────── */

static void temperature_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_DS18B20_READ_INTERVAL_MS);

    while (1) {
        /* If bus enumeration has been lost (e.g. sensor disconnected and
         * reconnected), try to re-detect devices before the next read. */
        if (s_device_count == 0) {
            ESP_LOGW(TAG, "No DS18B20 active – recreating bus and re-scanning ...");
            /* Recreate the bus first: a previous RMT timeout leaves the RX
             * channel disabled; a fresh bus handle resets the channel state. */
            recreate_bus();
            scan_devices();
            if (s_device_count == 0) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_valid = false;
                xSemaphoreGive(s_mutex);
                vTaskDelay(interval);
                continue;
            }
            ESP_LOGI(TAG, "Re-scan found %d DS18B20 device(s)", s_device_count);
            /* Reset fault state so alerts can fire again if needed */
            s_fail_count = 0;
            s_alert_sent = false;
        }

        /* Trigger conversion on all devices sharing the bus */
        esp_err_t err = ds18b20_trigger_temperature_conversion(s_devices[0]);
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
                /* Force a re-scan on the next cycle so that a reconnected
                 * sensor is picked up automatically. */
                s_device_count = 0;
            }
            vTaskDelay(interval);
            continue;
        }

        /* Read the first sensor (primary water probe) */
        float temp = 0.0f;
        err = ds18b20_get_temperature(s_devices[0], &temp);
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
                /* Force a re-scan on the next cycle. */
                s_device_count = 0;
            }
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

    /* 1. Pre-configure GPIO for ESP32-P4 compatibility.
     *    The RMT RX path requires the input buffer on the pad to be enabled
     *    before onewire_new_bus_rmt() installs the RX channel.  On ESP32-P4
     *    the GPIO input is not enabled by default; calling gpio_config() with
     *    GPIO_MODE_INPUT_OUTPUT_OD enables the input path and also activates
     *    the internal pull-up as a supplement to the external 4.7 kΩ resistor.
     */
    esp_err_t err;
    {
        gpio_config_t io_conf = {
            .pin_bit_mask  = (1ULL << CONFIG_DS18B20_GPIO),
            .mode          = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en    = GPIO_PULLUP_ENABLE,
            .pull_down_en  = GPIO_PULLDOWN_DISABLE,
            .intr_type     = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO pre-config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* 2. Create the 1-Wire bus using the RMT peripheral */
    err = recreate_bus();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "1-Wire bus created on GPIO %d", CONFIG_DS18B20_GPIO);

    /* 3. Enumerate DS18B20 devices – retry if none found immediately.
     *    After a power-on or soft reset the sensor may need a few hundred
     *    milliseconds before its 1-Wire ROM is accessible.
     *    Between attempts the bus is deleted and recreated: a timeout leaves
     *    the RMT RX channel in a disabled state; only a fresh bus handle
     *    resets the channel back to enabled. */
    for (int attempt = 0; attempt < SENSOR_DETECT_RETRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "No DS18B20 found – retry %d/%d in %d ms ...",
                     attempt, SENSOR_DETECT_RETRIES - 1, SENSOR_DETECT_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(SENSOR_DETECT_DELAY_MS));
            recreate_bus();   /* ignore error – scan_devices will also fail and loop continues */
        }
        if (scan_devices() > 0) {
            break;
        }
    }

    if (s_device_count == 0) {
        ESP_LOGW(TAG, "No DS18B20 found at startup on GPIO %d – "
                 "background task will keep retrying", CONFIG_DS18B20_GPIO);
    } else {
        ESP_LOGI(TAG, "Total DS18B20 devices: %d", s_device_count);
    }

    /* 4. Start periodic reading task.
     *    Always started so that the reconnection loop inside the task can
     *    detect a sensor that was absent (or not yet ready) at boot time.
     * Stack sized for RMT/1-Wire driver stack usage + logging + telegram calls. */
    BaseType_t ret = xTaskCreate(temperature_task, "ds18b20",
                                 4096, NULL, 5, NULL);
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
    /* Module not initialised – sensor probe failed at startup. */
    if (s_mutex == NULL) {
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
