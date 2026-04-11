/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - DuckDNS Dynamic DNS Client implementation
 * Background FreeRTOS task periodically updates a DuckDNS domain so
 * that the controller can be reached from outside the local network.
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
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "duckdns.h"
#include "wifi_manager.h"

static const char *TAG = "duckdns";

/* ── Constants ───────────────────────────────────────────────────── */

#define NVS_NAMESPACE    "duckdns"
#define NVS_KEY_DOMAIN   "domain"
#define NVS_KEY_TOKEN    "token"
#define NVS_KEY_ENABLED  "enabled"

#define TASK_STACK_SIZE  8192
#define UPDATE_PERIOD_MS (5 * 60 * 1000)   /* 5 minutes */

/* DuckDNS HTTPS update URL – documented at www.duckdns.org */
#define DUCKDNS_URL_FMT  "https://www.duckdns.org/update?domains=%s&token=%s&verbose=true"
#define URL_BUF_SIZE     256
#define RESP_BUF_SIZE    256

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex    = NULL;
static duckdns_config_t  s_config;
static TaskHandle_t      s_task     = NULL;
static char              s_last_status[64] = "never";

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    memset(&s_config, 0, sizeof(s_config));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved config – using defaults");
        return;
    }

    size_t len;

    len = sizeof(s_config.domain);
    if (nvs_get_str(h, NVS_KEY_DOMAIN, s_config.domain, &len) != ESP_OK) {
        s_config.domain[0] = '\0';
    }

    len = sizeof(s_config.token);
    if (nvs_get_str(h, NVS_KEY_TOKEN, s_config.token, &len) != ESP_OK) {
        s_config.token[0] = '\0';
    }

    uint8_t u8val;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &u8val) == ESP_OK) {
        s_config.enabled = u8val;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: enabled=%d domain_set=%d",
             s_config.enabled, s_config.domain[0] != '\0');
}

static esp_err_t nvs_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(h, NVS_KEY_DOMAIN, s_config.domain);
    nvs_set_str(h, NVS_KEY_TOKEN, s_config.token);
    nvs_set_u8(h, NVS_KEY_ENABLED, (uint8_t)s_config.enabled);
    nvs_commit(h);
    nvs_close(h);

    return ESP_OK;
}

/* ── DuckDNS HTTP update ─────────────────────────────────────────── */

/**
 * @brief Perform a single DuckDNS update request.
 *
 * The DuckDNS API is called via HTTPS GET with domain and token as
 * query parameters.  When verbose=true the response body is:
 *   Line 1: "OK" or "KO"
 *   Line 2: public IP (if OK)
 *   Line 3: IPv6 (or empty)
 *   Line 4: "UPDATED" or "NOCHANGE"
 */
static esp_err_t do_update(const char *domain, const char *token)
{
    char url[URL_BUF_SIZE];
    int url_len = snprintf(url, sizeof(url), DUCKDNS_URL_FMT, domain, token);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        ESP_LOGE(TAG, "URL too long");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_last_status, sizeof(s_last_status), "error: init failed");
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_last_status, sizeof(s_last_status), "error: %s",
                 esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    (void)content_length;

    char resp[RESP_BUF_SIZE];
    int read_len = esp_http_client_read(client, resp, sizeof(resp) - 1);
    if (read_len < 0) {
        read_len = 0;
    }
    resp[read_len] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "DuckDNS response (HTTP %d): %.*s",
             status, read_len > 80 ? 80 : read_len, resp);

    /* Parse first line of verbose response */
    bool ok = (strncmp(resp, "OK", 2) == 0);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) {
        /* Try to extract the IP from line 2 */
        const char *nl = strchr(resp, '\n');
        if (nl && *(nl + 1) != '\0') {
            const char *ip_start = nl + 1;
            const char *ip_end = strchr(ip_start, '\n');
            size_t ip_len = ip_end ? (size_t)(ip_end - ip_start)
                                   : strlen(ip_start);
            if (ip_len >= sizeof(s_last_status)) {
                ip_len = sizeof(s_last_status) - 5;
            }
            snprintf(s_last_status, sizeof(s_last_status),
                     "OK %.*s", (int)ip_len, ip_start);
        } else {
            snprintf(s_last_status, sizeof(s_last_status), "OK");
        }
    } else {
        snprintf(s_last_status, sizeof(s_last_status), "KO");
    }
    xSemaphoreGive(s_mutex);

    return ok ? ESP_OK : ESP_FAIL;
}

/* ── Background task ─────────────────────────────────────────────── */

static void duckdns_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool enabled = s_config.enabled;
        bool configured = (s_config.domain[0] != '\0' &&
                           s_config.token[0] != '\0');
        char domain[sizeof(s_config.domain)];
        char token[sizeof(s_config.token)];
        if (configured) {
            memcpy(domain, s_config.domain, sizeof(domain));
            memcpy(token, s_config.token, sizeof(token));
        }
        xSemaphoreGive(s_mutex);

        if (enabled && configured && wifi_manager_is_connected()) {
            do_update(domain, token);
        }

        vTaskDelay(pdMS_TO_TICKS(UPDATE_PERIOD_MS));
    }
}

static void start_task(void)
{
    if (s_task != NULL) {
        return;   /* already running */
    }
    xTaskCreate(duckdns_task, "duckdns", TASK_STACK_SIZE, NULL, 3, &s_task);
    ESP_LOGI(TAG, "Background update task started (every %d s)",
             UPDATE_PERIOD_MS / 1000);
}

static void stop_task(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
        ESP_LOGI(TAG, "Background update task stopped");
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t duckdns_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();

    if (s_config.enabled && s_config.domain[0] != '\0'
        && s_config.token[0] != '\0') {
        start_task();
    }

    return ESP_OK;
}

duckdns_config_t duckdns_get_config(void)
{
    duckdns_config_t copy;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy = s_config;
    xSemaphoreGive(s_mutex);
    return copy;
}

esp_err_t duckdns_set_config(const duckdns_config_t *cfg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = *cfg;
    xSemaphoreGive(s_mutex);

    esp_err_t err = nvs_save_config();
    if (err != ESP_OK) {
        return err;
    }

    /* (Re)start or stop the task based on the new config */
    stop_task();
    if (cfg->enabled && cfg->domain[0] != '\0' && cfg->token[0] != '\0') {
        start_task();
    }

    return ESP_OK;
}

esp_err_t duckdns_update_now(void)
{
    duckdns_config_t cfg = duckdns_get_config();

    if (cfg.domain[0] == '\0' || cfg.token[0] == '\0') {
        ESP_LOGW(TAG, "Cannot update – domain or token not configured");
        return ESP_ERR_INVALID_STATE;
    }

    return do_update(cfg.domain, cfg.token);
}

const char *duckdns_get_last_status(void)
{
    /* Returning the static buffer is safe for read; callers must not
     * free it or write to it. */
    return s_last_status;
}
