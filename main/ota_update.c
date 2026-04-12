/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - OTA Firmware Update implementation
 * Background FreeRTOS task downloads and flashes a firmware image.
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
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"

#include "ota_update.h"

static const char *TAG = "ota";

/* ── Constants ───────────────────────────────────────────────────── */

#define OTA_TASK_STACK    12288
#define OTA_BUF_SIZE      4096
#define OTA_TIMEOUT_MS    30000
#define OTA_URL_MAX       256

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex       = NULL;
static ota_progress_t    s_progress    = { .status = OTA_STATUS_IDLE };
static bool              s_in_progress = false;
static char              s_url[OTA_URL_MAX];

/* ── OTA task ────────────────────────────────────────────────────── */

static void ota_task(void *arg)
{
    (void)arg;

    /* Update status: downloading */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.status       = OTA_STATUS_DOWNLOADING;
    s_progress.progress_pct = 0;
    s_progress.error_msg[0] = '\0';
    char url_copy[OTA_URL_MAX];
    strncpy(url_copy, s_url, sizeof(url_copy) - 1);
    url_copy[sizeof(url_copy) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Starting OTA from: %s", url_copy);

    /* Configure HTTP client */
    esp_http_client_config_t http_cfg = {
        .url               = url_copy,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = OTA_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "HTTP client init failed");
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Failed to init HTTP client");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "HTTP open: %s", esp_err_to_name(err));
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int total_size = content_length > 0 ? content_length : 0;

    /* Prepare OTA partition */
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "No OTA partition found");
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "No OTA partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Writing to partition '%s' at offset 0x%"PRIx32,
             update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                        &ota_handle);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "OTA begin: %s", esp_err_to_name(err));
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    /* Download and flash loop */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.status = OTA_STATUS_FLASHING;
    xSemaphoreGive(s_mutex);

    char *buf = malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "OOM");
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int bytes_read = 0;
    bool success = true;
    while (1) {
        int len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (len < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            success = false;
            break;
        }
        if (len == 0) {
            /* Check if all data received when content-length known */
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            /* Incomplete but connection closed */
            if (total_size > 0 && bytes_read < total_size) {
                ESP_LOGE(TAG, "Incomplete download: %d/%d", bytes_read, total_size);
                success = false;
            }
            break;
        }

        err = esp_ota_write(ota_handle, buf, (size_t)len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            success = false;
            break;
        }

        bytes_read += len;
        if (total_size > 0) {
            int pct = (int)((int64_t)bytes_read * 100 / total_size);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_progress.progress_pct = pct > 100 ? 100 : pct;
            xSemaphoreGive(s_mutex);
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!success) {
        esp_ota_abort(ota_handle);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "Download/flash failed");
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "OTA end: %s", esp_err_to_name(err));
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "Set boot partition: %s", esp_err_to_name(err));
        s_in_progress = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA update successful! (%d bytes) – rebooting in 3 s …",
             bytes_read);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.status       = OTA_STATUS_DONE;
    s_progress.progress_pct = 100;
    s_in_progress           = false;
    xSemaphoreGive(s_mutex);

    /* Give the web UI time to poll the final status */
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t ota_update_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Lazy-init mutex */
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_in_progress) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_in_progress = true;
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_progress.status       = OTA_STATUS_DOWNLOADING;
    s_progress.progress_pct = 0;
    s_progress.error_msg[0] = '\0';
    xSemaphoreGive(s_mutex);

    BaseType_t ret = xTaskCreate(ota_task, "ota_update",
                                 OTA_TASK_STACK, NULL, 5, NULL);
    if (ret != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_in_progress = false;
        s_progress.status = OTA_STATUS_ERROR;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg),
                 "Task creation failed");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

ota_progress_t ota_update_get_progress(void)
{
    ota_progress_t p;
    if (s_mutex == NULL) {
        memset(&p, 0, sizeof(p));
        return p;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    p = s_progress;
    xSemaphoreGive(s_mutex);
    return p;
}

bool ota_update_in_progress(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool in_prog = s_in_progress;
    xSemaphoreGive(s_mutex);
    return in_prog;
}
