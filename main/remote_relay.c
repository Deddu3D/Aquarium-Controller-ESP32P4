/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Zero-Config MQTT Remote Relay implementation
 *
 * Establishes an outbound TLS MQTT connection to a managed public broker
 * (broker.hivemq.com:8883) so the controller is reachable from anywhere
 * without port-forwarding, DuckDNS, or any user account.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "remote_relay.h"
#include "led_controller.h"
#include "temperature_sensor.h"
#include "relay_controller.h"
#include "feeding_mode.h"

static const char *TAG = "remote_relay";

/* ── Configuration ───────────────────────────────────────────────── */

#ifndef CONFIG_REMOTE_RELAY_BROKER_URI
#define CONFIG_REMOTE_RELAY_BROKER_URI "mqtts://broker.hivemq.com:8883"
#endif

#define TOPIC_PREFIX        "aquarium/"
#define TOPIC_STATUS_SUFFIX "/status"
#define TOPIC_CMD_SUFFIX    "/cmd"
#define TOPIC_RESP_SUFFIX   "/response"

/* Maximum full topic length: "aquarium/" + 12 + "/response" + NUL */
#define TOPIC_BUF_SIZE      64

#define STATUS_JSON_SIZE    512

/* QoS 1 for commands and responses; QoS 0 for periodic status */
#define QOS_STATUS  0
#define QOS_CMD     1
#define QOS_RESP    1

/* ── Private state ───────────────────────────────────────────────── */

static char               s_device_id[REMOTE_RELAY_DEVICE_ID_LEN] = "";
static char               s_topic_status[TOPIC_BUF_SIZE];
static char               s_topic_cmd[TOPIC_BUF_SIZE];
static char               s_topic_response[TOPIC_BUF_SIZE];

static esp_mqtt_client_handle_t s_client    = NULL;
static SemaphoreHandle_t        s_mutex     = NULL;
static bool                     s_connected = false;

/* ── Minimal JSON helpers ────────────────────────────────────────── */

/**
 * @brief Find a numeric value for a JSON key.
 * Returns 0 on success, –1 if not found.
 */
static int rr_json_get_double(const char *json, const char *key, double *out)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '\0') return -1;
    char *end;
    double v = strtod(p, &end);
    if (end == p) return -1;
    *out = v;
    return 0;
}

/**
 * @brief Find a boolean value (true/false) for a JSON key.
 * Returns 1 (true), 0 (false), or –1 (not found).
 */
static int rr_json_get_bool(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0)  return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

/* ── Status JSON builder ─────────────────────────────────────────── */

static int build_status_json(char *buf, size_t size)
{
    float temp_c  = 0.0f;
    bool  temp_ok = temperature_sensor_get(&temp_c);

    bool relay[4];
    for (int i = 0; i < 4; i++) {
        relay[i] = relay_controller_get(i);
    }

    bool    led_on  = led_controller_is_on();
    uint8_t led_bri = led_controller_get_brightness();
    bool    feeding = feeding_mode_is_active();

    int64_t  uptime_s = esp_timer_get_time() / 1000000LL;
    uint32_t heap     = esp_get_free_heap_size();

    return snprintf(buf, size,
        "{"
        "\"type\":\"status\","
        "\"temp_c\":%.2f,"
        "\"temp_ok\":%s,"
        "\"uptime_s\":%" PRId64 ","
        "\"free_heap\":%" PRIu32 ","
        "\"relay_0\":%s,"
        "\"relay_1\":%s,"
        "\"relay_2\":%s,"
        "\"relay_3\":%s,"
        "\"led_on\":%s,"
        "\"led_brightness\":%u,"
        "\"feeding_active\":%s,"
        "\"remote_connected\":true"
        "}",
        (double)temp_c,
        temp_ok ? "true" : "false",
        uptime_s,
        heap,
        relay[0] ? "true" : "false",
        relay[1] ? "true" : "false",
        relay[2] ? "true" : "false",
        relay[3] ? "true" : "false",
        led_on ? "true" : "false",
        (unsigned)led_bri,
        feeding ? "true" : "false"
    );
}

/* ── Command dispatcher ──────────────────────────────────────────── */

static void dispatch_command(const char *data, int data_len)
{
    /* Null-terminate for string operations */
    char buf[512];
    size_t copy_len = (size_t)data_len < sizeof(buf) - 1
                      ? (size_t)data_len : sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    ESP_LOGI(TAG, "Command received (%d bytes)", data_len);

    /* Extract "cmd" string field */
    const char *cmd_p = strstr(buf, "\"cmd\"");
    if (!cmd_p) {
        ESP_LOGW(TAG, "Missing 'cmd' field in command payload");
        return;
    }
    cmd_p += 5; /* skip "cmd" */
    while (*cmd_p == ' ' || *cmd_p == ':') cmd_p++;
    if (*cmd_p == '"') cmd_p++;

    char cmd[32] = {0};
    for (int i = 0; i < (int)(sizeof(cmd) - 1) && *cmd_p && *cmd_p != '"'; i++) {
        cmd[i] = *cmd_p++;
    }

    char resp_buf[STATUS_JSON_SIZE];
    int resp_len = 0;
    bool publish_status = true;

    if (strcmp(cmd, "get_status") == 0) {
        /* Just trigger a full status publish – handled below */

    } else if (strcmp(cmd, "set_led") == 0) {
        double dval;
        int bval = rr_json_get_bool(buf, "\"on\"");
        if (bval == 1)       led_controller_on();
        else if (bval == 0)  led_controller_off();

        if (rr_json_get_double(buf, "\"brightness\"", &dval) == 0) {
            uint8_t bri = (uint8_t)(dval < 0 ? 0 : dval > 255 ? 255 : dval);
            led_controller_set_brightness(bri);
        }
        if (rr_json_get_double(buf, "\"r\"", &dval) == 0) {
            double g = 0.0, b = 0.0;
            rr_json_get_double(buf, "\"g\"", &g);
            rr_json_get_double(buf, "\"b\"", &b);
            uint8_t r8 = (uint8_t)(dval  < 0 ? 0 : dval  > 255 ? 255 : dval);
            uint8_t g8 = (uint8_t)(g     < 0 ? 0 : g     > 255 ? 255 : g);
            uint8_t b8 = (uint8_t)(b     < 0 ? 0 : b     > 255 ? 255 : b);
            led_controller_set_color(r8, g8, b8);
        }
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"set_led\",\"ok\":true}");

    } else if (strcmp(cmd, "relay_toggle") == 0) {
        double idx_d = -1.0;
        if (rr_json_get_double(buf, "\"index\"", &idx_d) == 0) {
            int idx  = (int)idx_d;
            int bval = rr_json_get_bool(buf, "\"on\"");
            if (idx >= 0 && idx < 4 && bval >= 0) {
                relay_controller_set(idx, bval == 1);
            }
        }
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"relay_toggle\",\"ok\":true}");

    } else if (strcmp(cmd, "feeding_start") == 0) {
        feeding_mode_start();
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"feeding_start\",\"ok\":true}");

    } else if (strcmp(cmd, "feeding_stop") == 0) {
        feeding_mode_stop();
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"feeding_stop\",\"ok\":true}");

    } else if (strcmp(cmd, "get_temperature") == 0) {
        float temp_c = 0.0f;
        bool ok = temperature_sensor_get(&temp_c);
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"get_temperature\",\"valid\":%s,"
                            "\"temperature_c\":%.2f}",
                            ok ? "true" : "false", (double)temp_c);
        publish_status = false;

    } else if (strcmp(cmd, "get_relays") == 0) {
        bool r[4];
        for (int i = 0; i < 4; i++) r[i] = relay_controller_get(i);
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"get_relays\","
                            "\"relay_0\":%s,"
                            "\"relay_1\":%s,"
                            "\"relay_2\":%s,"
                            "\"relay_3\":%s}",
                            r[0] ? "true" : "false",
                            r[1] ? "true" : "false",
                            r[2] ? "true" : "false",
                            r[3] ? "true" : "false");
        publish_status = false;

    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
        resp_len = snprintf(resp_buf, sizeof(resp_buf),
                            "{\"cmd\":\"%s\",\"ok\":false,"
                            "\"error\":\"unknown_cmd\"}", cmd);
        publish_status = false;
    }

    /* Publish command response (if any) */
    if (resp_len > 0 && s_client) {
        esp_mqtt_client_publish(s_client, s_topic_response,
                                resp_buf, resp_len, QOS_RESP, 0);
    }

    /* Publish updated status after state-changing commands */
    if (publish_status) {
        remote_relay_publish_status();
    }
}

/* ── MQTT event handler ──────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connected = true;
        xSemaphoreGive(s_mutex);

        /* Subscribe to the command topic */
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, QOS_CMD);
        ESP_LOGI(TAG, "Subscribed to %s", s_topic_cmd);

        /* Publish an initial status so the app knows the device is online */
        remote_relay_publish_status();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected – will auto-reconnect");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connected = false;
        xSemaphoreGive(s_mutex);
        break;

    case MQTT_EVENT_DATA:
        dispatch_command(event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type %d",
                 (int)event->error_handle->error_type);
        break;

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t remote_relay_init(void)
{
#ifndef CONFIG_REMOTE_RELAY_ENABLE
    ESP_LOGI(TAG, "Remote relay disabled in Kconfig – skipping");
    return ESP_OK;
#endif

    /* ── Derive device ID from base MAC ────────────────────────── */
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC: %s", esp_err_to_name(err));
        /* Fall back to a placeholder so topics are still valid */
        snprintf(s_device_id, sizeof(s_device_id), "000000000000");
    } else {
        snprintf(s_device_id, sizeof(s_device_id),
                 "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);

    /* ── Build topic strings ────────────────────────────────────── */
    snprintf(s_topic_status,   sizeof(s_topic_status),
             TOPIC_PREFIX "%s" TOPIC_STATUS_SUFFIX,   s_device_id);
    snprintf(s_topic_cmd,      sizeof(s_topic_cmd),
             TOPIC_PREFIX "%s" TOPIC_CMD_SUFFIX,      s_device_id);
    snprintf(s_topic_response, sizeof(s_topic_response),
             TOPIC_PREFIX "%s" TOPIC_RESP_SUFFIX,     s_device_id);

    /* ── Create module mutex ────────────────────────────────────── */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* ── Configure and start MQTT client ───────────────────────── */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_REMOTE_RELAY_BROKER_URI,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            /* Use device_id as client ID to avoid broker conflicts */
            .client_id = s_device_id,
        },
        .session = {
            .keepalive             = 60,
            .disable_clean_session = false,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MQTT remote relay started → %s", CONFIG_REMOTE_RELAY_BROKER_URI);
    ESP_LOGI(TAG, "Status topic : %s", s_topic_status);
    ESP_LOGI(TAG, "Command topic: %s", s_topic_cmd);

    return ESP_OK;
}

void remote_relay_get_device_id(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_device_id, len - 1);
    buf[len - 1] = '\0';
}

bool remote_relay_is_connected(void)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool c = s_connected;
    xSemaphoreGive(s_mutex);
    return c;
}

void remote_relay_publish_status(void)
{
    if (!s_client) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool connected = s_connected;
    xSemaphoreGive(s_mutex);

    if (!connected) return;

    char json[STATUS_JSON_SIZE];
    int len = build_status_json(json, sizeof(json));
    if (len > 0) {
        esp_mqtt_client_publish(s_client, s_topic_status, json, len,
                                QOS_STATUS, 0);
        ESP_LOGD(TAG, "Status published (%d bytes)", len);
    }
}
