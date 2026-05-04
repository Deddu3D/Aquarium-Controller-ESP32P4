/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - SD Data Logger implementation
 * Appends records to daily rotating CSV/log files on the SD card.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "sd_logger.h"
#include "sd_card.h"

static const char *TAG = "sd_log";

/* Mutex protects file access from multiple tasks */
static SemaphoreHandle_t s_mutex = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * Format a UNIX timestamp as ISO-8601 string: "YYYY-MM-DDTHH:MM:SSZ"
 */
static void fmt_iso(time_t ts, char *buf, size_t len)
{
    struct tm ti;
    gmtime_r(&ts, &ti);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &ti);
}

/**
 * Format a daily file path like /sdcard/logs/<prefix>_YYYYMMDD.<ext>
 */
static void make_daily_path(time_t ts, const char *prefix,
                             const char *ext, char *out, size_t out_len)
{
    struct tm ti;
    localtime_r(&ts, &ti);
    snprintf(out, out_len, SD_LOGS_DIR "/%s_%04d%02d%02d.%s",
             prefix,
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ext);
}

/**
 * Open a file for appending, writing a header line if it is new.
 */
static FILE *open_daily_file(const char *path, const char *header)
{
    bool is_new = false;
    struct stat st;
    if (stat(path, &st) != 0) {
        is_new = true;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGW(TAG, "Cannot open %s for appending", path);
        return NULL;
    }

    if (is_new && header != NULL) {
        fprintf(f, "%s\n", header);
    }

    return f;
}

/** Sanitise a string for CSV: replace commas and newlines with spaces. */
static void sanitise_csv(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; i++) {
        char c = src[i];
        dst[j++] = (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[j] = '\0';
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t sd_logger_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (sd_card_is_mounted()) {
        ESP_LOGI(TAG, "SD logger ready");
    } else {
        ESP_LOGW(TAG, "SD card not mounted – logging disabled until card is available");
    }
    return ESP_OK;
}

void sd_logger_log_temperature(time_t ts, float temp_c)
{
    if (!sd_card_is_mounted() || s_mutex == NULL) {
        return;
    }

    char path[64];
    make_daily_path(ts, "temp", "csv", path, sizeof(path));

    char iso[24];
    fmt_iso(ts, iso, sizeof(iso));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FILE *f = open_daily_file(path, "timestamp,temperature_c");
    if (f) {
        fprintf(f, "%s,%.2f\n", iso, temp_c);
        fclose(f);
    }
    xSemaphoreGive(s_mutex);
}

void sd_logger_log_event(time_t ts, const char *type, const char *detail)
{
    if (!sd_card_is_mounted() || s_mutex == NULL) {
        return;
    }
    if (type == NULL) {
        type = "unknown";
    }

    char path[64];
    make_daily_path(ts, "events", "csv", path, sizeof(path));

    char iso[24];
    fmt_iso(ts, iso, sizeof(iso));

    char type_s[32];
    char detail_s[128];
    sanitise_csv(type,          type_s,   sizeof(type_s));
    sanitise_csv(detail ? detail : "", detail_s, sizeof(detail_s));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FILE *f = open_daily_file(path, "timestamp,type,detail");
    if (f) {
        fprintf(f, "%s,%s,%s\n", iso, type_s, detail_s);
        fclose(f);
    }
    xSemaphoreGive(s_mutex);
}

void sd_logger_log_telegram(time_t ts, const char *message)
{
    if (!sd_card_is_mounted() || s_mutex == NULL) {
        return;
    }

    char path[64];
    make_daily_path(ts, "telegram", "log", path, sizeof(path));

    char iso[24];
    fmt_iso(ts, iso, sizeof(iso));

    char msg_s[256];
    sanitise_csv(message ? message : "", msg_s, sizeof(msg_s));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FILE *f = open_daily_file(path, "timestamp,message");
    if (f) {
        fprintf(f, "%s,%s\n", iso, msg_s);
        fclose(f);
    }
    xSemaphoreGive(s_mutex);
}

void sd_logger_log_diagnostic(time_t ts, const char *level,
                               const char *tag, const char *message)
{
    if (!sd_card_is_mounted() || s_mutex == NULL) {
        return;
    }

    char path[64];
    make_daily_path(ts, "diag", "log", path, sizeof(path));

    char iso[24];
    fmt_iso(ts, iso, sizeof(iso));

    char tag_s[32];
    char msg_s[256];
    sanitise_csv(tag     ? tag     : "", tag_s, sizeof(tag_s));
    sanitise_csv(message ? message : "", msg_s, sizeof(msg_s));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FILE *f = open_daily_file(path, "timestamp,level,tag,message");
    if (f) {
        fprintf(f, "%s,%s,%s,%s\n",
                iso,
                (level && *level) ? level : "?",
                tag_s,
                msg_s);
        fclose(f);
    }
    xSemaphoreGive(s_mutex);
}
