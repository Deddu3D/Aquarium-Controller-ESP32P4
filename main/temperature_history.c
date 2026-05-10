/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Temperature History implementation
 * Ring buffer that stores 24 hours of temperature samples taken
 * every CONFIG_TEMP_HISTORY_INTERVAL_SEC seconds (default 300 = 5 min).
 *
 * NVS persistence: the ring buffer is saved to NVS every
 * TEMP_HIST_NVS_SAVE_INTERVAL samples so that history survives reboots.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "temperature_sensor.h"
#include "temperature_history.h"

static const char *TAG = "temp_hist";

/* Task parameters */
#define HISTORY_TASK_STACK  3072
#define HISTORY_TASK_PRIO   4

/* NVS persistence */
#define HIST_NVS_NAMESPACE "temp_hist"
#define HIST_NVS_KEY_DATA  "ring"
/* Save to NVS every N new samples (6 × 5 min = every 30 min by default) */
#define TEMP_HIST_NVS_SAVE_INTERVAL 6

/* Serialised form kept in NVS */
typedef struct {
    int32_t      head;
    int32_t      count;
    temp_sample_t samples[TEMP_HISTORY_MAX_SAMPLES];
} temp_hist_nvs_t;

/* ── Ring buffer ─────────────────────────────────────────────────── */

static temp_sample_t     s_ring[TEMP_HISTORY_MAX_SAMPLES];
static int               s_head;          /* next write position   */
static int               s_count;         /* valid entries stored  */
static SemaphoreHandle_t s_mutex;

/* ── NVS helpers ─────────────────────────────────────────────────── */

/** Save ring buffer state to NVS.  Caller must hold s_mutex. */
static void nvs_save_ring(void)
{
    nvs_handle_t h;
    if (nvs_open(HIST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed – skipping save");
        return;
    }

    temp_hist_nvs_t blob;
    blob.head  = (int32_t)s_head;
    blob.count = (int32_t)s_count;
    memcpy(blob.samples, s_ring, sizeof(s_ring));

    esp_err_t err = nvs_set_blob(h, HIST_NVS_KEY_DATA, &blob, sizeof(blob));
    if (err == ESP_OK) {
        nvs_commit(h);
        ESP_LOGD(TAG, "Saved %d samples to NVS", s_count);
    } else {
        ESP_LOGW(TAG, "NVS blob write failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

/** Load ring buffer state from NVS.  Caller must hold s_mutex. */
static void nvs_load_ring(void)
{
    nvs_handle_t h;
    if (nvs_open(HIST_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No NVS history found – starting fresh");
        return;
    }

    temp_hist_nvs_t blob;
    size_t blob_size = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, HIST_NVS_KEY_DATA, &blob, &blob_size);
    nvs_close(h);

    if (err != ESP_OK || blob_size != sizeof(blob)) {
        ESP_LOGI(TAG, "NVS history unreadable – starting fresh");
        return;
    }

    /* Sanity-check the loaded metadata */
    if (blob.count < 0 || blob.count > TEMP_HISTORY_MAX_SAMPLES ||
        blob.head  < 0 || blob.head  >= TEMP_HISTORY_MAX_SAMPLES) {
        ESP_LOGW(TAG, "NVS history corrupt – discarding");
        return;
    }

    s_head  = (int)blob.head;
    s_count = (int)blob.count;
    memcpy(s_ring, blob.samples, sizeof(s_ring));
    ESP_LOGI(TAG, "Restored %d samples from NVS", s_count);
}

/* ── Sampling task ───────────────────────────────────────────────── */

static void history_task(void *arg)
{
    (void)arg;
    const TickType_t interval =
        pdMS_TO_TICKS((uint32_t)CONFIG_TEMP_HISTORY_INTERVAL_SEC * 1000U);
    int samples_since_save = 0;

    while (1) {
        float temp_c = 0.0f;
        if (temperature_sensor_get(&temp_c)) {
            time_t now = time(NULL);
            /* Only record if the system clock looks valid (year >= 2024) */
            struct tm ti;
            localtime_r(&now, &ti);
            if (ti.tm_year >= (2024 - 1900)) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_ring[s_head].timestamp = now;
                s_ring[s_head].temp_c    = temp_c;
                s_head = (s_head + 1) % TEMP_HISTORY_MAX_SAMPLES;
                if (s_count < TEMP_HISTORY_MAX_SAMPLES) {
                    s_count++;
                }
                samples_since_save++;
                bool do_save = (samples_since_save >= TEMP_HIST_NVS_SAVE_INTERVAL);
                if (do_save) {
                    nvs_save_ring();
                    samples_since_save = 0;
                }
                xSemaphoreGive(s_mutex);
                ESP_LOGD(TAG, "Recorded %.2f °C  (count=%d)", temp_c, s_count);
            }
        }
        vTaskDelay(interval);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t temperature_history_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(s_ring, 0, sizeof(s_ring));
    s_head  = 0;
    s_count = 0;

    /* Restore ring buffer persisted in NVS (survives reboots) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_load_ring();
    xSemaphoreGive(s_mutex);

    BaseType_t ret = xTaskCreate(history_task, "temp_hist",
                                 HISTORY_TASK_STACK, NULL,
                                 HISTORY_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create history task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Temperature history started (interval %d s, max %d samples)",
             CONFIG_TEMP_HISTORY_INTERVAL_SEC, TEMP_HISTORY_MAX_SAMPLES);
    return ESP_OK;
}

void temperature_history_get(temp_sample_t *out, int *out_count)
{
    if (out == NULL || out_count == NULL) {
        return;
    }

    /* Module not yet initialised (e.g. DS18B20 probe failed) – return
     * an empty result set instead of crashing on a NULL semaphore.     */
    if (s_mutex == NULL) {
        *out_count = 0;
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Return samples in chronological order (oldest first).
     * The oldest sample sits at (s_head - s_count) mod size. */
    int start = (s_head - s_count + TEMP_HISTORY_MAX_SAMPLES)
                % TEMP_HISTORY_MAX_SAMPLES;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % TEMP_HISTORY_MAX_SAMPLES;
        out[i] = s_ring[idx];
    }
    *out_count = s_count;

    xSemaphoreGive(s_mutex);
}
