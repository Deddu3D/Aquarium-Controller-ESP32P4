/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller - SD Card Manager implementation
 * Mounts a FAT32 microSD card via the SDMMC peripheral (SDMMC_HOST_SLOT_1,
 * 1-bit mode) and provides configuration backup / restore via JSON.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 *
 * Hardware wiring (Waveshare ESP32-P4-WiFi6 onboard TF slot, SDMMC 1-bit):
 *   SDMMC1_CLK = GPIO 43   (configurable via Kconfig)
 *   SDMMC1_CMD = GPIO 44
 *   SDMMC1_D0  = GPIO 39
 *   SDMMC1_D3  = GPIO 38   (card-detect pull-up, configurable via Kconfig)
 *
 * Note: SDMMC_HOST_SLOT_0 uses fixed IOMUX pads (GPIO 14-19) and is already
 * claimed by the esp_hosted WiFi coprocessor SDIO transport.
 * SDMMC_HOST_SLOT_1 is GPIO-matrix-routable and is free for the SD card.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "cJSON.h"

#include "sd_card.h"

/* Module headers needed for config export / import */
#include "auto_heater.h"
#include "co2_controller.h"
#include "feeding_mode.h"
#include "daily_cycle.h"
#include "led_schedule.h"
#include "led_scenes.h"
#include "relay_controller.h"
#include "telegram_notify.h"
#include "duckdns.h"
#include "timezone_manager.h"

static const char *TAG = "sd_card";

/* ── Private state ───────────────────────────────────────────────── */

/** FAT32 sector size in bytes (standard for SD cards). */
#define SD_FAT_SECTOR_SIZE   512U

/** How many times to attempt mounting before giving up. */
#define SD_MOUNT_RETRIES     3
/** Milliseconds to wait between mount retry attempts. */
#define SD_RETRY_DELAY_MS    500

static sdmmc_card_t      *s_card        = NULL;
static bool               s_mounted     = false;
static SemaphoreHandle_t  s_mutex       = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

/** Ensure a directory exists (creates if absent). */
static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t sd_card_init(void)
{
#ifndef CONFIG_SD_CARD_ENABLED
    ESP_LOGI(TAG, "SD card disabled in Kconfig – skipping");
    return ESP_ERR_NOT_FOUND;
#else
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_mounted) {
        ESP_LOGW(TAG, "Already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initialising SD card (SDMMC1, 1-bit, CLK=%d CMD=%d D0=%d D3=%d)",
             CONFIG_SD_CLK_GPIO, CONFIG_SD_CMD_GPIO,
             CONFIG_SD_D0_GPIO, CONFIG_SD_D3_GPIO);

    /* SDMMC_HOST_SLOT_1 uses the GPIO matrix – pin assignment below.
     * SDMMC_HOST_SLOT_0 is occupied by the esp_hosted WiFi SDIO (GPIO 14-19). */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_1;
    /* Start at a conservative frequency; the driver will renegotiate higher
     * speeds (up to 40 MHz) after card identification succeeds. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  /* 20 MHz */

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;                          /* 1-bit mode */
    slot_config.clk   = CONFIG_SD_CLK_GPIO;         /* GPIO 43 */
    slot_config.cmd   = CONFIG_SD_CMD_GPIO;         /* GPIO 44 */
    slot_config.d0    = CONFIG_SD_D0_GPIO;          /* GPIO 39 */
    slot_config.d3    = CONFIG_SD_D3_GPIO;          /* GPIO 38 – pull-up for card-detect */
    /* Enable the ESP32-P4 on-chip pull-ups on all SD lines so the bus idles
     * high when the card is not driving it (prevents spurious CMD0 timeouts). */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_RETRIES; attempt++) {
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT,
                                      &host,
                                      &slot_config,
                                      &mount_config,
                                      &s_card);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "Mount attempt %d/%d failed (%s)%s",
                 attempt, SD_MOUNT_RETRIES, esp_err_to_name(ret),
                 attempt < SD_MOUNT_RETRIES ? " – retrying" : "");
        if (attempt < SD_MOUNT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
        }
    }

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem – card present?");
        } else {
            ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);

    /* Create mandatory subdirectories */
    ensure_dir(SD_LOGS_DIR);
    ensure_dir(SD_CONFIG_DIR);
    ensure_dir(SD_WWW_DIR);

    ESP_LOGI(TAG, "SD card mounted at " SD_MOUNT_POINT " (name=%s speed=%"PRIu32" kHz)",
             s_card->cid.name,
             s_card->max_freq_khz);

    return ESP_OK;
#endif /* CONFIG_SD_CARD_ENABLED */
}

void sd_card_deinit(void)
{
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

void sd_card_get_info(sd_card_info_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));

    if (!s_mounted || s_card == NULL) {
        return;
    }

    info->mounted        = true;
    info->total_bytes    = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    info->card_speed_khz = s_card->max_freq_khz;
    strncpy(info->card_name, s_card->cid.name, sizeof(info->card_name) - 1);

    /* Query FAT free space */
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
        uint64_t free_clust_bytes = (uint64_t)fre_clust *
                                    fs->csize *
                                    SD_FAT_SECTOR_SIZE;
        info->free_bytes = free_clust_bytes;
    }
}

/* ── Config export / import ──────────────────────────────────────── */

esp_err_t sd_card_config_export(const char *path)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "SD card not mounted – cannot export config");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "version", 1);
    {
        time_t now = time(NULL);
        char ts[32];
        struct tm ti;
        gmtime_r(&now, &ti);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &ti);
        cJSON_AddStringToObject(root, "exported", ts);
    }

    /* ── LED Schedule ── */
    led_schedule_config_t lsched = led_schedule_get_config();
    cJSON *ls = cJSON_CreateObject();
    cJSON_AddBoolToObject(ls, "enabled",          lsched.enabled);
    cJSON_AddNumberToObject(ls, "on_hour",         lsched.on_hour);
    cJSON_AddNumberToObject(ls, "on_minute",       lsched.on_minute);
    cJSON_AddNumberToObject(ls, "ramp_duration_min", lsched.ramp_duration_min);
    cJSON_AddBoolToObject(ls, "pause_enabled",     lsched.pause_enabled);
    cJSON_AddNumberToObject(ls, "pause_start_hour",  lsched.pause_start_hour);
    cJSON_AddNumberToObject(ls, "pause_start_minute",lsched.pause_start_minute);
    cJSON_AddNumberToObject(ls, "pause_end_hour",  lsched.pause_end_hour);
    cJSON_AddNumberToObject(ls, "pause_end_minute",lsched.pause_end_minute);
    cJSON_AddNumberToObject(ls, "pause_brightness",lsched.pause_brightness);
    cJSON_AddNumberToObject(ls, "pause_red",       lsched.pause_red);
    cJSON_AddNumberToObject(ls, "pause_green",     lsched.pause_green);
    cJSON_AddNumberToObject(ls, "pause_blue",      lsched.pause_blue);
    cJSON_AddNumberToObject(ls, "off_hour",        lsched.off_hour);
    cJSON_AddNumberToObject(ls, "off_minute",      lsched.off_minute);
    cJSON_AddNumberToObject(ls, "brightness",      lsched.brightness);
    cJSON_AddNumberToObject(ls, "red",             lsched.red);
    cJSON_AddNumberToObject(ls, "green",           lsched.green);
    cJSON_AddNumberToObject(ls, "blue",            lsched.blue);
    cJSON_AddItemToObject(root, "led_schedule", ls);

    /* ── LED Scenes ── */
    led_scenes_config_t lscene = led_scenes_get_config();
    cJSON *sc = cJSON_CreateObject();
    cJSON_AddNumberToObject(sc, "sunrise_duration_min",   lscene.sunrise_duration_min);
    cJSON_AddNumberToObject(sc, "sunrise_max_brightness", lscene.sunrise_max_brightness);
    cJSON_AddNumberToObject(sc, "sunset_duration_min",    lscene.sunset_duration_min);
    cJSON_AddNumberToObject(sc, "moonlight_brightness",   lscene.moonlight_brightness);
    cJSON_AddNumberToObject(sc, "moonlight_r",            lscene.moonlight_r);
    cJSON_AddNumberToObject(sc, "moonlight_g",            lscene.moonlight_g);
    cJSON_AddNumberToObject(sc, "moonlight_b",            lscene.moonlight_b);
    cJSON_AddNumberToObject(sc, "storm_intensity",        lscene.storm_intensity);
    cJSON_AddNumberToObject(sc, "clouds_depth",           lscene.clouds_depth);
    cJSON_AddNumberToObject(sc, "clouds_period_s",        lscene.clouds_period_s);
    cJSON_AddItemToObject(root, "led_scenes", sc);

    /* ── Auto-Heater ── */
    auto_heater_config_t ahcfg = auto_heater_get_config();
    cJSON *ah = cJSON_CreateObject();
    cJSON_AddBoolToObject(ah,   "enabled",       ahcfg.enabled);
    cJSON_AddNumberToObject(ah, "relay_index",   ahcfg.relay_index);
    cJSON_AddNumberToObject(ah, "target_temp_c", ahcfg.target_temp_c);
    cJSON_AddNumberToObject(ah, "hysteresis_c",  ahcfg.hysteresis_c);
    cJSON_AddItemToObject(root, "auto_heater", ah);

    /* ── CO2 Controller ── */
    co2_config_t co2cfg = co2_controller_get_config();
    cJSON *co2 = cJSON_CreateObject();
    cJSON_AddBoolToObject(co2,   "enabled",      co2cfg.enabled);
    cJSON_AddNumberToObject(co2, "relay_index",  co2cfg.relay_index);
    cJSON_AddNumberToObject(co2, "pre_on_min",   co2cfg.pre_on_min);
    cJSON_AddNumberToObject(co2, "post_off_min", co2cfg.post_off_min);
    cJSON_AddItemToObject(root, "co2", co2);

    /* ── Feeding Mode ── */
    feeding_config_t fcfg = feeding_mode_get_config();
    cJSON *fm = cJSON_CreateObject();
    cJSON_AddNumberToObject(fm, "relay_index",   fcfg.relay_index);
    cJSON_AddNumberToObject(fm, "duration_min",  fcfg.duration_min);
    cJSON_AddBoolToObject(fm,   "dim_lights",    fcfg.dim_lights);
    cJSON_AddNumberToObject(fm, "dim_brightness",fcfg.dim_brightness);
    cJSON_AddItemToObject(root, "feeding", fm);

    /* ── Relay names & schedules ── */
    cJSON *relays = cJSON_CreateArray();
    relay_state_t rstates[RELAY_COUNT];
    relay_controller_get_all(rstates);
    for (int i = 0; i < RELAY_COUNT; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", rstates[i].name);
        cJSON *slots = cJSON_CreateArray();
        for (int s = 0; s < RELAY_SCHEDULE_SLOTS; s++) {
            cJSON *slot = cJSON_CreateObject();
            cJSON_AddBoolToObject(slot,   "enabled", rstates[i].schedules[s].enabled);
            cJSON_AddNumberToObject(slot, "on_min",  rstates[i].schedules[s].on_min);
            cJSON_AddNumberToObject(slot, "off_min", rstates[i].schedules[s].off_min);
            cJSON_AddItemToArray(slots, slot);
        }
        cJSON_AddItemToObject(r, "schedules", slots);
        cJSON_AddItemToArray(relays, r);
    }
    cJSON_AddItemToObject(root, "relays", relays);

    /* ── Telegram ── */
    telegram_config_t tgcfg = telegram_notify_get_config();
    cJSON *tg = cJSON_CreateObject();
    /* Include credentials – file is local on SD card */
    cJSON_AddStringToObject(tg,  "bot_token",            tgcfg.bot_token);
    cJSON_AddStringToObject(tg,  "chat_id",              tgcfg.chat_id);
    cJSON_AddBoolToObject(tg,    "enabled",              tgcfg.enabled);
    cJSON_AddBoolToObject(tg,    "temp_alarm_enabled",   tgcfg.temp_alarm_enabled);
    cJSON_AddNumberToObject(tg,  "temp_high_c",          tgcfg.temp_high_c);
    cJSON_AddNumberToObject(tg,  "temp_low_c",           tgcfg.temp_low_c);
    cJSON_AddBoolToObject(tg,    "water_change_enabled", tgcfg.water_change_enabled);
    cJSON_AddNumberToObject(tg,  "water_change_days",    tgcfg.water_change_days);
    cJSON_AddBoolToObject(tg,    "fertilizer_enabled",   tgcfg.fertilizer_enabled);
    cJSON_AddNumberToObject(tg,  "fertilizer_days",      tgcfg.fertilizer_days);
    cJSON_AddBoolToObject(tg,    "daily_summary_enabled",tgcfg.daily_summary_enabled);
    cJSON_AddNumberToObject(tg,  "daily_summary_hour",   tgcfg.daily_summary_hour);
    cJSON_AddBoolToObject(tg,    "relay_notify_enabled", tgcfg.relay_notify_enabled);
    cJSON_AddItemToObject(root, "telegram", tg);

    /* ── DuckDNS ── */
    duckdns_config_t ddcfg = duckdns_get_config();
    cJSON *dd = cJSON_CreateObject();
    cJSON_AddStringToObject(dd, "domain",  ddcfg.domain);
    cJSON_AddStringToObject(dd, "token",   ddcfg.token);
    cJSON_AddBoolToObject(dd,   "enabled", ddcfg.enabled);
    cJSON_AddItemToObject(root, "duckdns", dd);

    /* ── Daily Cycle ── */
    daily_cycle_config_t dccfg = daily_cycle_get_config();
    cJSON *dc = cJSON_CreateObject();
    cJSON_AddBoolToObject(dc,   "enabled",   dccfg.enabled);
    cJSON_AddNumberToObject(dc, "latitude",  dccfg.latitude);
    cJSON_AddNumberToObject(dc, "longitude", dccfg.longitude);
    cJSON_AddItemToObject(root, "daily_cycle", dc);

    /* ── Timezone ── */
    char tz_str[TZ_STRING_MAX];
    timezone_manager_get(tz_str, sizeof(tz_str));
    cJSON_AddStringToObject(root, "timezone", tz_str);

    /* Serialise to string */
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Write to file */
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        free(json_str);
        return ESP_FAIL;
    }

    size_t written = fwrite(json_str, 1, strlen(json_str), f);
    fclose(f);
    free(json_str);

    if (written == 0) {
        ESP_LOGE(TAG, "Write to %s failed", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config exported to %s (%u bytes)", path, (unsigned)written);
    return ESP_OK;
}

esp_err_t sd_card_config_import(const char *path)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "SD card not mounted – cannot import config");
        return ESP_ERR_INVALID_STATE;
    }

    /* Read file into a heap buffer */
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot open %s for reading", path);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0 || file_size > 32768) {
        fclose(f);
        ESP_LOGE(TAG, "Config file size invalid (%ld bytes)", file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)file_size + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    buf[file_size] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse config JSON");
        return ESP_FAIL;
    }

    /* ── LED Schedule ── */
    cJSON *ls = cJSON_GetObjectItem(root, "led_schedule");
    if (ls) {
        led_schedule_config_t cfg = led_schedule_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(ls, "enabled")))           cfg.enabled           = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "on_hour")))           cfg.on_hour           = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "on_minute")))         cfg.on_minute         = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "ramp_duration_min"))) cfg.ramp_duration_min = (uint16_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_enabled")))     cfg.pause_enabled     = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_start_hour")))  cfg.pause_start_hour  = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_start_minute")))cfg.pause_start_minute= (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_end_hour")))    cfg.pause_end_hour    = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_end_minute")))  cfg.pause_end_minute  = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_brightness")))  cfg.pause_brightness  = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_red")))         cfg.pause_red         = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_green")))       cfg.pause_green       = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "pause_blue")))        cfg.pause_blue        = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "off_hour")))          cfg.off_hour          = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "off_minute")))        cfg.off_minute        = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "brightness")))        cfg.brightness        = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "red")))               cfg.red               = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "green")))             cfg.green             = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(ls, "blue")))              cfg.blue              = (uint8_t)j->valueint;
        led_schedule_set_config(&cfg);
    }

    /* ── LED Scenes ── */
    cJSON *sc = cJSON_GetObjectItem(root, "led_scenes");
    if (sc) {
        led_scenes_config_t cfg = led_scenes_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(sc, "sunrise_duration_min")))   cfg.sunrise_duration_min   = (uint16_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "sunrise_max_brightness")))  cfg.sunrise_max_brightness  = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "sunset_duration_min")))     cfg.sunset_duration_min    = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "moonlight_brightness")))    cfg.moonlight_brightness   = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "moonlight_r")))             cfg.moonlight_r            = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "moonlight_g")))             cfg.moonlight_g            = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "moonlight_b")))             cfg.moonlight_b            = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "storm_intensity")))         cfg.storm_intensity        = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "clouds_depth")))            cfg.clouds_depth           = (uint8_t)j->valueint;
        if ((j = cJSON_GetObjectItem(sc, "clouds_period_s")))         cfg.clouds_period_s        = (uint16_t)j->valueint;
        led_scenes_set_config(&cfg);
    }

    /* ── Auto-Heater ── */
    cJSON *ah = cJSON_GetObjectItem(root, "auto_heater");
    if (ah) {
        auto_heater_config_t cfg = auto_heater_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(ah, "enabled")))       cfg.enabled       = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(ah, "relay_index")))   cfg.relay_index   = j->valueint;
        if ((j = cJSON_GetObjectItem(ah, "target_temp_c"))) cfg.target_temp_c = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(ah, "hysteresis_c")))  cfg.hysteresis_c  = (float)j->valuedouble;
        auto_heater_set_config(&cfg);
    }

    /* ── CO2 Controller ── */
    cJSON *co2 = cJSON_GetObjectItem(root, "co2");
    if (co2) {
        co2_config_t cfg = co2_controller_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(co2, "enabled")))      cfg.enabled      = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(co2, "relay_index")))  cfg.relay_index  = j->valueint;
        if ((j = cJSON_GetObjectItem(co2, "pre_on_min")))   cfg.pre_on_min   = j->valueint;
        if ((j = cJSON_GetObjectItem(co2, "post_off_min"))) cfg.post_off_min = j->valueint;
        co2_controller_set_config(&cfg);
    }

    /* ── Feeding Mode ── */
    cJSON *fm = cJSON_GetObjectItem(root, "feeding");
    if (fm) {
        feeding_config_t cfg = feeding_mode_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(fm, "relay_index")))    cfg.relay_index    = j->valueint;
        if ((j = cJSON_GetObjectItem(fm, "duration_min")))   cfg.duration_min   = j->valueint;
        if ((j = cJSON_GetObjectItem(fm, "dim_lights")))     cfg.dim_lights     = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(fm, "dim_brightness"))) cfg.dim_brightness = (uint8_t)j->valueint;
        feeding_mode_set_config(&cfg);
    }

    /* ── Relay names & schedules ── */
    cJSON *relays = cJSON_GetObjectItem(root, "relays");
    if (relays && cJSON_IsArray(relays)) {
        int n = cJSON_GetArraySize(relays);
        for (int i = 0; i < n && i < RELAY_COUNT; i++) {
            cJSON *r = cJSON_GetArrayItem(relays, i);
            if (r == NULL) continue;
            cJSON *j = cJSON_GetObjectItem(r, "name");
            if (j && cJSON_IsString(j)) {
                relay_controller_set_name(i, j->valuestring);
            }
            cJSON *slots = cJSON_GetObjectItem(r, "schedules");
            if (slots && cJSON_IsArray(slots)) {
                int ns = cJSON_GetArraySize(slots);
                for (int s = 0; s < ns && s < RELAY_SCHEDULE_SLOTS; s++) {
                    cJSON *slot = cJSON_GetArrayItem(slots, s);
                    if (slot == NULL) continue;
                    relay_schedule_t sched = {0};
                    cJSON *se = cJSON_GetObjectItem(slot, "enabled");
                    cJSON *so = cJSON_GetObjectItem(slot, "on_min");
                    cJSON *sf = cJSON_GetObjectItem(slot, "off_min");
                    if (se) sched.enabled = (bool)se->valueint;
                    if (so) sched.on_min  = (uint16_t)so->valueint;
                    if (sf) sched.off_min = (uint16_t)sf->valueint;
                    relay_controller_set_schedule(i, s, &sched);
                }
            }
        }
    }

    /* ── Telegram ── */
    cJSON *tg = cJSON_GetObjectItem(root, "telegram");
    if (tg) {
        telegram_config_t cfg = telegram_notify_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(tg, "bot_token")) && cJSON_IsString(j))
            strncpy(cfg.bot_token, j->valuestring, sizeof(cfg.bot_token) - 1);
        if ((j = cJSON_GetObjectItem(tg, "chat_id")) && cJSON_IsString(j))
            strncpy(cfg.chat_id, j->valuestring, sizeof(cfg.chat_id) - 1);
        if ((j = cJSON_GetObjectItem(tg, "enabled")))              cfg.enabled              = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "temp_alarm_enabled")))   cfg.temp_alarm_enabled   = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "temp_high_c")))          cfg.temp_high_c          = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(tg, "temp_low_c")))           cfg.temp_low_c           = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(tg, "water_change_enabled"))) cfg.water_change_enabled = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "water_change_days")))    cfg.water_change_days    = j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "fertilizer_enabled")))   cfg.fertilizer_enabled   = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "fertilizer_days")))      cfg.fertilizer_days      = j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "daily_summary_enabled")))cfg.daily_summary_enabled= (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "daily_summary_hour")))   cfg.daily_summary_hour   = j->valueint;
        if ((j = cJSON_GetObjectItem(tg, "relay_notify_enabled"))) cfg.relay_notify_enabled = (bool)j->valueint;
        telegram_notify_set_config(&cfg);
    }

    /* ── DuckDNS ── */
    cJSON *dd = cJSON_GetObjectItem(root, "duckdns");
    if (dd) {
        duckdns_config_t cfg = duckdns_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(dd, "domain")) && cJSON_IsString(j))
            strncpy(cfg.domain, j->valuestring, sizeof(cfg.domain) - 1);
        if ((j = cJSON_GetObjectItem(dd, "token")) && cJSON_IsString(j))
            strncpy(cfg.token, j->valuestring, sizeof(cfg.token) - 1);
        if ((j = cJSON_GetObjectItem(dd, "enabled"))) cfg.enabled = (bool)j->valueint;
        duckdns_set_config(&cfg);
    }

    /* ── Daily Cycle ── */
    cJSON *dc = cJSON_GetObjectItem(root, "daily_cycle");
    if (dc) {
        daily_cycle_config_t cfg = daily_cycle_get_config();
        cJSON *j;
        if ((j = cJSON_GetObjectItem(dc, "enabled")))   cfg.enabled   = (bool)j->valueint;
        if ((j = cJSON_GetObjectItem(dc, "latitude")))  cfg.latitude  = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(dc, "longitude"))) cfg.longitude = (float)j->valuedouble;
        daily_cycle_set_config(&cfg);
    }

    /* ── Timezone ── */
    cJSON *tz = cJSON_GetObjectItem(root, "timezone");
    if (tz && cJSON_IsString(tz)) {
        timezone_manager_set(tz->valuestring);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config imported from %s", path);
    return ESP_OK;
}
