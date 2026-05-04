/*
 * SPDX-License-Identifier: MIT
 *
 * Aquarium Controller – Voice Control via Groq Cloud API
 *
 * Records audio from an INMP441 I2S microphone, sends it to the
 * Groq Whisper STT endpoint for Italian transcription, then passes
 * the transcript to a Groq LLM to produce a structured JSON command
 * that is executed locally on the aquarium controller.
 *
 * Target board : Waveshare ESP32-P4-WiFi6 rev 1.3
 * ESP-IDF      : v6.0.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Status ──────────────────────────────────────────────────────── */

/**
 * @brief Processing pipeline status.
 */
typedef enum {
    VOICE_STATUS_IDLE        = 0, /**< Waiting for trigger             */
    VOICE_STATUS_RECORDING   = 1, /**< Capturing audio from I2S mic    */
    VOICE_STATUS_TRANSCRIBING = 2,/**< Uploading audio to Groq Whisper */
    VOICE_STATUS_PROCESSING  = 3, /**< Sending transcript to Groq LLM  */
    VOICE_STATUS_DONE        = 4, /**< Command executed successfully    */
    VOICE_STATUS_ERROR       = 5, /**< Pipeline failed (see last_error) */
} voice_status_t;

/* ── Configuration ───────────────────────────────────────────────── */

/**
 * @brief Voice-control run-time configuration (NVS-persisted).
 */
typedef struct {
    char groq_api_key[128]; /**< Groq API key (sk-...)                  */
    bool enabled;           /**< Master on/off switch                   */
    int  record_ms;         /**< Recording duration in milliseconds     */
    char stt_model[48];     /**< Groq Whisper model name                */
    char llm_model[48];     /**< Groq chat-completions model name       */
    int  i2s_sck_io;        /**< I2S bit-clock GPIO (BCLK)              */
    int  i2s_ws_io;         /**< I2S word-select GPIO (LRCLK)           */
    int  i2s_sd_io;         /**< I2S data GPIO (SD / DIN from INMP441)  */
} voice_config_t;

/* ── Public API ──────────────────────────────────────────────────── */

/**
 * @brief Initialise the voice-control module.
 *
 * Loads configuration from NVS (or applies Kconfig defaults).
 * Does NOT open the I2S peripheral; that happens on demand when
 * a recording is triggered to avoid holding DMA buffers permanently.
 *
 * Must be called after nvs_flash_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t voice_control_init(void);

/**
 * @brief Trigger an asynchronous recording + transcription + execution cycle.
 *
 * Spawns a one-shot FreeRTOS task that:
 *   1. Opens I2S and records for @c record_ms milliseconds.
 *   2. Converts the raw PCM to a 16-bit WAV.
 *   3. POSTs the WAV to the Groq Whisper API.
 *   4. Passes the Italian transcript to the Groq LLM.
 *   5. Parses the JSON command and executes it.
 *
 * Returns ESP_ERR_INVALID_STATE if a recording is already in progress,
 * the module is disabled, or the Groq API key has not been configured.
 *
 * @return ESP_OK if the task was spawned successfully.
 */
esp_err_t voice_control_start_record(void);

/**
 * @brief Get the current pipeline status (thread-safe).
 */
voice_status_t voice_control_get_status(void);

/**
 * @brief Copy the last Italian transcript into @p buf (thread-safe).
 *
 * @param buf  Destination buffer.
 * @param len  Size of @p buf in bytes.
 */
void voice_control_get_transcript(char *buf, size_t len);

/**
 * @brief Copy the last JSON command string into @p buf (thread-safe).
 *
 * @param buf  Destination buffer.
 * @param len  Size of @p buf in bytes.
 */
void voice_control_get_last_command(char *buf, size_t len);

/**
 * @brief Copy a human-readable result string into @p buf (thread-safe).
 *
 * @param buf  Destination buffer.
 * @param len  Size of @p buf in bytes.
 */
void voice_control_get_last_result(char *buf, size_t len);

/**
 * @brief Return the current configuration (thread-safe copy).
 */
voice_config_t voice_control_get_config(void);

/**
 * @brief Update the configuration and persist to NVS.
 *
 * @param cfg  New configuration to apply.
 * @return ESP_OK on success.
 */
esp_err_t voice_control_set_config(const voice_config_t *cfg);

#ifdef __cplusplus
}
#endif
