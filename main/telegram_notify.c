/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - Telegram Notification Service implementation
 * Background FreeRTOS task monitors temperature thresholds,
 * sends maintenance reminders and daily summaries via Telegram Bot API.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "telegram_notify.h"
#include "temperature_sensor.h"
#include "led_scenes.h"
#include "led_controller.h"

static const char *TAG = "telegram";

/* ── Constants ───────────────────────────────────────────────────── */

#define NVS_NAMESPACE      "telegram"
#define NVS_KEY_TOKEN      "token"
#define NVS_KEY_CHAT_ID    "chat_id"
#define NVS_KEY_ENABLED    "enabled"
#define NVS_KEY_TALM_EN    "t_alm_en"
#define NVS_KEY_T_HIGH     "t_high"
#define NVS_KEY_T_LOW      "t_low"
#define NVS_KEY_WC_EN      "wc_en"
#define NVS_KEY_WC_DAYS    "wc_days"
#define NVS_KEY_FERT_EN    "fert_en"
#define NVS_KEY_FERT_DAYS  "fert_days"
#define NVS_KEY_SUM_EN     "sum_en"
#define NVS_KEY_SUM_HOUR   "sum_hour"
#define NVS_KEY_LAST_WC    "last_wc"
#define NVS_KEY_LAST_FERT  "last_fert"

#define TASK_STACK_SIZE    12288   /* TLS handshake needs generous stack */
#define TASK_PERIOD_MS     60000   /* Check every 60 seconds */
#define TEMP_ALARM_COOLDOWN_S  1800  /* 30 minutes between repeated alarms */

#define TELEGRAM_API_URL   "https://api.telegram.org/bot"
#define MSG_BUF_SIZE       1024
#define SEND_MAX_RETRIES   2       /* Retry once with fallback cert method */

/* Embedded root CA certificates for api.telegram.org
 * Contains Go Daddy Root CA G2 + DigiCert Global Root G2.           */
extern const char telegram_root_cert_pem_start[] asm("_binary_telegram_root_cert_pem_start");
extern const char telegram_root_cert_pem_end[]   asm("_binary_telegram_root_cert_pem_end");

/* ── Private state ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex   = NULL;
static telegram_config_t s_config;
static int64_t s_last_water_change  = 0;
static int64_t s_last_fertilizer    = 0;

/* Alarm cooldown tracking */
static int64_t s_last_temp_high_alarm_time = 0;
static int64_t s_last_temp_low_alarm_time  = 0;
static int     s_last_summary_day          = -1; /* day-of-year for dedup */

/* ── NVS helpers ─────────────────────────────────────────────────── */

static void nvs_load_config(void)
{
    /* Set defaults */
    memset(&s_config, 0, sizeof(s_config));
    s_config.temp_high_c         = 30.0f;
    s_config.temp_low_c          = 20.0f;
    s_config.water_change_days   = 7;
    s_config.fertilizer_days     = 7;
    s_config.daily_summary_hour  = 8;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved config – using defaults");
        return;
    }

    size_t len;

    len = sizeof(s_config.bot_token);
    if (nvs_get_str(h, NVS_KEY_TOKEN, s_config.bot_token, &len) != ESP_OK) {
        s_config.bot_token[0] = '\0';
    }

    len = sizeof(s_config.chat_id);
    if (nvs_get_str(h, NVS_KEY_CHAT_ID, s_config.chat_id, &len) != ESP_OK) {
        s_config.chat_id[0] = '\0';
    }

    uint8_t u8val;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &u8val) == ESP_OK)
        s_config.enabled = u8val;
    if (nvs_get_u8(h, NVS_KEY_TALM_EN, &u8val) == ESP_OK)
        s_config.temp_alarm_enabled = u8val;
    if (nvs_get_u8(h, NVS_KEY_WC_EN, &u8val) == ESP_OK)
        s_config.water_change_enabled = u8val;
    if (nvs_get_u8(h, NVS_KEY_FERT_EN, &u8val) == ESP_OK)
        s_config.fertilizer_enabled = u8val;
    if (nvs_get_u8(h, NVS_KEY_SUM_EN, &u8val) == ESP_OK)
        s_config.daily_summary_enabled = u8val;

    int32_t i32val;
    if (nvs_get_i32(h, NVS_KEY_T_HIGH, &i32val) == ESP_OK)
        s_config.temp_high_c = (float)i32val / 100.0f;
    if (nvs_get_i32(h, NVS_KEY_T_LOW, &i32val) == ESP_OK)
        s_config.temp_low_c = (float)i32val / 100.0f;
    if (nvs_get_i32(h, NVS_KEY_WC_DAYS, &i32val) == ESP_OK)
        s_config.water_change_days = (int)i32val;
    if (nvs_get_i32(h, NVS_KEY_FERT_DAYS, &i32val) == ESP_OK)
        s_config.fertilizer_days = (int)i32val;
    if (nvs_get_i32(h, NVS_KEY_SUM_HOUR, &i32val) == ESP_OK)
        s_config.daily_summary_hour = (int)i32val;

    int64_t i64val;
    if (nvs_get_i64(h, NVS_KEY_LAST_WC, &i64val) == ESP_OK)
        s_last_water_change = i64val;
    if (nvs_get_i64(h, NVS_KEY_LAST_FERT, &i64val) == ESP_OK)
        s_last_fertilizer = i64val;

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: enabled=%d token_set=%d chat_id_set=%d",
             s_config.enabled,
             s_config.bot_token[0] != '\0',
             s_config.chat_id[0] != '\0');
}

static esp_err_t nvs_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(h, NVS_KEY_TOKEN, s_config.bot_token);
    nvs_set_str(h, NVS_KEY_CHAT_ID, s_config.chat_id);
    nvs_set_u8(h, NVS_KEY_ENABLED, (uint8_t)s_config.enabled);
    nvs_set_u8(h, NVS_KEY_TALM_EN, (uint8_t)s_config.temp_alarm_enabled);
    nvs_set_u8(h, NVS_KEY_WC_EN, (uint8_t)s_config.water_change_enabled);
    nvs_set_u8(h, NVS_KEY_FERT_EN, (uint8_t)s_config.fertilizer_enabled);
    nvs_set_u8(h, NVS_KEY_SUM_EN, (uint8_t)s_config.daily_summary_enabled);
    nvs_set_i32(h, NVS_KEY_T_HIGH, (int32_t)(s_config.temp_high_c * 100.0f));
    nvs_set_i32(h, NVS_KEY_T_LOW, (int32_t)(s_config.temp_low_c * 100.0f));
    nvs_set_i32(h, NVS_KEY_WC_DAYS, (int32_t)s_config.water_change_days);
    nvs_set_i32(h, NVS_KEY_FERT_DAYS, (int32_t)s_config.fertilizer_days);
    nvs_set_i32(h, NVS_KEY_SUM_HOUR, (int32_t)s_config.daily_summary_hour);
    nvs_set_i64(h, NVS_KEY_LAST_WC, s_last_water_change);
    nvs_set_i64(h, NVS_KEY_LAST_FERT, s_last_fertilizer);
    nvs_commit(h);
    nvs_close(h);

    return ESP_OK;
}

/* ── JSON escaping for Telegram messages ─────────────────────────── */

/**
 * @brief Escape a string for safe embedding in a JSON value.
 *
 * Handles double-quotes, backslashes, and newlines.
 */
static void json_msg_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 3 < dst_size; i++) {
        switch (src[i]) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\r': break;  /* skip carriage returns */
        default:   dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
}

/* ── Telegram HTTP sender ────────────────────────────────────────── */

static esp_err_t send_telegram_message(const char *token, const char *chat_id,
                                       const char *text)
{
    /* Guard: TLS certificate validation requires a valid system clock.
     * If SNTP has not synchronised yet the handshake will fail with
     * MBEDTLS_ERR_X509_CERT_VERIFY_FAILED (-0x3000).                   */
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    if (ti.tm_year < (2024 - 1900)) {
        ESP_LOGE(TAG, "System clock not synchronised (year=%d) – "
                 "cannot verify TLS certificates. "
                 "Waiting for SNTP sync …", ti.tm_year + 1900);
        return ESP_ERR_INVALID_STATE;
    }

    /* Build URL: https://api.telegram.org/bot<token>/sendMessage */
    char url[256];
    int url_len = snprintf(url, sizeof(url), "%s%s/sendMessage",
                           TELEGRAM_API_URL, token);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL too long");
        return ESP_ERR_NO_MEM;
    }

    /* Escape text for JSON embedding */
    char *escaped = malloc(MSG_BUF_SIZE);
    if (escaped == NULL) {
        return ESP_ERR_NO_MEM;
    }
    json_msg_escape(text, escaped, MSG_BUF_SIZE);

    /* Build JSON body */
    char *body = malloc(MSG_BUF_SIZE);
    if (body == NULL) {
        free(escaped);
        return ESP_ERR_NO_MEM;
    }

    snprintf(body, MSG_BUF_SIZE,
        "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\"}",
        chat_id, escaped);
    free(escaped);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .cert_pem = telegram_root_cert_pem_start,
        .timeout_ms = 10000,
    };

    esp_err_t err = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; attempt < SEND_MAX_RETRIES; attempt++) {
        if (attempt == 1) {
            /* Second attempt: fall back to the built-in certificate bundle
             * in case Telegram migrated to a CA not in the embedded PEM.   */
            ESP_LOGW(TAG, "Retrying with certificate bundle …");
            http_cfg.cert_pem = NULL;
            http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (client == NULL) {
            free(body);
            return ESP_FAIL;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));

        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);

        esp_http_client_cleanup(client);

        if (err == ESP_OK) {
            break;   /* success – no need to retry */
        }

        ESP_LOGE(TAG, "HTTP request failed (attempt %d/%d): %s",
                 attempt + 1, SEND_MAX_RETRIES, esp_err_to_name(err));
    }

    free(body);

    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "Telegram API returned HTTP %d", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Telegram message sent successfully");
    return ESP_OK;
}

/* ── Notification builders ───────────────────────────────────────── */

static void check_temperature_alarms(const telegram_config_t *cfg)
{
    if (!cfg->temp_alarm_enabled) return;

    float temp_c;
    if (!temperature_sensor_get(&temp_c)) return;

    time_t now = time(NULL);

    /* High temperature alarm */
    if (temp_c > cfg->temp_high_c &&
        (now - s_last_temp_high_alarm_time) > TEMP_ALARM_COOLDOWN_S) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "\xf0\x9f\x9a\xa8 <b>Temperature ALARM</b>\n\n"
            "\xf0\x9f\x8c\xa1 Current: <b>%.1f\xc2\xb0"
            "C</b>\n"
            "\xe2\x9a\xa0\xef\xb8\x8f Threshold: %.1f\xc2\xb0"
            "C\n\n"
            "Water temperature is <b>too high</b>!",
            temp_c, cfg->temp_high_c);
        if (send_telegram_message(cfg->bot_token, cfg->chat_id, msg) == ESP_OK) {
            s_last_temp_high_alarm_time = now;
        }
    }

    /* Low temperature alarm */
    if (temp_c < cfg->temp_low_c &&
        (now - s_last_temp_low_alarm_time) > TEMP_ALARM_COOLDOWN_S) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "\xf0\x9f\x9a\xa8 <b>Temperature ALARM</b>\n\n"
            "\xf0\x9f\x8c\xa1 Current: <b>%.1f\xc2\xb0"
            "C</b>\n"
            "\xe2\x9a\xa0\xef\xb8\x8f Threshold: %.1f\xc2\xb0"
            "C\n\n"
            "Water temperature is <b>too low</b>!",
            temp_c, cfg->temp_low_c);
        if (send_telegram_message(cfg->bot_token, cfg->chat_id, msg) == ESP_OK) {
            s_last_temp_low_alarm_time = now;
        }
    }
}

static void send_daily_summary(const telegram_config_t *cfg)
{
    char msg[512];
    int pos = 0;

    pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
        "\xf0\x9f\x90\x9f <b>Aquarium Daily Summary</b>\n\n");

    /* Temperature */
    float temp_c;
    if (temperature_sensor_get(&temp_c)) {
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
            "\xf0\x9f\x8c\xa1 Temperature: <b>%.1f\xc2\xb0"
            "C</b>\n", temp_c);
    } else {
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
            "\xf0\x9f\x8c\xa1 Temperature: <i>no sensor</i>\n");
    }

    /* LED scene */
    pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
        "\xf0\x9f\x92\xa1 Scene: <b>%s</b>\n",
        led_scenes_get_name(led_scenes_get()));

    /* Water change status */
    time_t now = time(NULL);
    if (cfg->water_change_enabled && s_last_water_change > 0) {
        int days_ago = (int)((now - s_last_water_change) / 86400);
        int days_left = cfg->water_change_days - days_ago;
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
            "\xf0\x9f\x92\xa7 Water change: %d days ago", days_ago);
        if (days_left <= 0) {
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                " \xe2\x9a\xa0\xef\xb8\x8f <b>OVERDUE</b>\n");
        } else {
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                " (due in %d days)\n", days_left);
        }
    }

    /* Fertilizer status */
    if (cfg->fertilizer_enabled && s_last_fertilizer > 0) {
        int days_ago = (int)((now - s_last_fertilizer) / 86400);
        int days_left = cfg->fertilizer_days - days_ago;
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
            "\xf0\x9f\x8c\xbf Fertilizer: %d days ago", days_ago);
        if (days_left <= 0) {
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                " \xe2\x9a\xa0\xef\xb8\x8f <b>OVERDUE</b>\n");
        } else {
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                " (due in %d days)\n", days_left);
        }
    }

    /* Uptime */
    int64_t up_s = esp_timer_get_time() / 1000000;
    int uh = (int)(up_s / 3600);
    int um = (int)((up_s % 3600) / 60);
    pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
        "\xe2\x8f\xb1 Uptime: %dh %dm\n", uh, um);

    (void)pos;   /* suppress unused-value warning */
    send_telegram_message(cfg->bot_token, cfg->chat_id, msg);
}

static void send_maintenance_reminders(const telegram_config_t *cfg)
{
    time_t now = time(NULL);

    /* Water change reminder */
    if (cfg->water_change_enabled && s_last_water_change > 0) {
        int days_since = (int)((now - s_last_water_change) / 86400);
        if (days_since >= cfg->water_change_days) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "\xf0\x9f\x92\xa7 <b>Water Change Reminder</b>\n\n"
                "It's been <b>%d days</b> since the last water change.\n"
                "Scheduled interval: every %d days.",
                days_since, cfg->water_change_days);
            send_telegram_message(cfg->bot_token, cfg->chat_id, msg);
        }
    }

    /* Fertilizer reminder */
    if (cfg->fertilizer_enabled && s_last_fertilizer > 0) {
        int days_since = (int)((now - s_last_fertilizer) / 86400);
        if (days_since >= cfg->fertilizer_days) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "\xf0\x9f\x8c\xbf <b>Fertilizer Reminder</b>\n\n"
                "It's been <b>%d days</b> since the last fertilizer dose.\n"
                "Scheduled interval: every %d days.",
                days_since, cfg->fertilizer_days);
            send_telegram_message(cfg->bot_token, cfg->chat_id, msg);
        }
    }
}

/* ── Background task ─────────────────────────────────────────────── */

static void telegram_task(void *arg)
{
    (void)arg;

    /* Wait a bit for WiFi / NTP to settle */
    vTaskDelay(pdMS_TO_TICKS(30000));

    while (1) {
        /* Read config snapshot under mutex */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        telegram_config_t cfg = s_config;
        xSemaphoreGive(s_mutex);

        if (cfg.enabled && cfg.bot_token[0] != '\0' && cfg.chat_id[0] != '\0') {
            /* Temperature alarms – checked every tick */
            check_temperature_alarms(&cfg);

            /* Time-based reminders and daily summary */
            time_t now = time(NULL);
            struct tm ti;
            localtime_r(&now, &ti);

            if (ti.tm_year >= (2024 - 1900)) {
                int today = ti.tm_yday;

                /* Run daily tasks once per day at the configured hour */
                if (ti.tm_hour == cfg.daily_summary_hour &&
                    today != s_last_summary_day) {
                    s_last_summary_day = today;

                    if (cfg.daily_summary_enabled) {
                        send_daily_summary(&cfg);
                    }
                    send_maintenance_reminders(&cfg);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t telegram_notify_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load_config();

    BaseType_t ret = xTaskCreate(telegram_task, "telegram",
                                 TASK_STACK_SIZE, NULL,
                                 tskIDLE_PRIORITY + 1, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create telegram task");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Telegram notification module initialised");
    return ESP_OK;
}

telegram_config_t telegram_notify_get_config(void)
{
    telegram_config_t cfg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);
    return cfg;
}

esp_err_t telegram_notify_set_config(const telegram_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    telegram_config_t safe = *cfg;

    /* Clamp values */
    if (safe.temp_high_c < -10.0f) safe.temp_high_c = -10.0f;
    if (safe.temp_high_c > 50.0f)  safe.temp_high_c = 50.0f;
    if (safe.temp_low_c < -10.0f)  safe.temp_low_c = -10.0f;
    if (safe.temp_low_c > 50.0f)   safe.temp_low_c = 50.0f;
    if (safe.water_change_days < 1)  safe.water_change_days = 1;
    if (safe.water_change_days > 90) safe.water_change_days = 90;
    if (safe.fertilizer_days < 1)    safe.fertilizer_days = 1;
    if (safe.fertilizer_days > 90)   safe.fertilizer_days = 90;
    if (safe.daily_summary_hour < 0)  safe.daily_summary_hour = 0;
    if (safe.daily_summary_hour > 23) safe.daily_summary_hour = 23;

    /* Null-terminate strings */
    safe.bot_token[sizeof(safe.bot_token) - 1] = '\0';
    safe.chat_id[sizeof(safe.chat_id) - 1] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = safe;
    xSemaphoreGive(s_mutex);

    return nvs_save_config();
}

esp_err_t telegram_notify_send(const char *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    telegram_config_t cfg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_mutex);

    if (cfg.bot_token[0] == '\0' || cfg.chat_id[0] == '\0') {
        ESP_LOGW(TAG, "Bot token or chat ID not configured");
        return ESP_ERR_INVALID_STATE;
    }

    return send_telegram_message(cfg.bot_token, cfg.chat_id, message);
}

esp_err_t telegram_notify_reset_water_change(void)
{
    time_t now = time(NULL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_water_change = (int64_t)now;
    xSemaphoreGive(s_mutex);

    /* Persist timestamp */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_i64(h, NVS_KEY_LAST_WC, (int64_t)now);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Water change recorded at %" PRId64, (int64_t)now);
    return ESP_OK;
}

esp_err_t telegram_notify_reset_fertilizer(void)
{
    time_t now = time(NULL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_fertilizer = (int64_t)now;
    xSemaphoreGive(s_mutex);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_i64(h, NVS_KEY_LAST_FERT, (int64_t)now);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Fertilizer dose recorded at %" PRId64, (int64_t)now);
    return ESP_OK;
}

int64_t telegram_notify_get_last_water_change(void)
{
    int64_t val;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    val = s_last_water_change;
    xSemaphoreGive(s_mutex);
    return val;
}

int64_t telegram_notify_get_last_fertilizer(void)
{
    int64_t val;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    val = s_last_fertilizer;
    xSemaphoreGive(s_mutex);
    return val;
}
