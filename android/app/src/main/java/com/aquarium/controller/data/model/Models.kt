package com.aquarium.controller.data.model

import com.squareup.moshi.Json
import com.squareup.moshi.JsonClass

@JsonClass(generateAdapter = true)
data class LoginRequest(val username: String, val password: String)

@JsonClass(generateAdapter = true)
data class LoginResponse(val ok: Boolean)

@JsonClass(generateAdapter = true)
data class AuthChangeRequest(val username: String, val password: String)

@JsonClass(generateAdapter = true)
data class StatusResponse(
    val connected: Boolean,
    val ip: String,
    val ssid: String,
    val rssi: Int,
    @Json(name = "free_heap") val freeHeap: Long,
    @Json(name = "uptime_s") val uptimeS: Long,
    @Json(name = "ntp_ok") val ntpOk: Boolean,
    val partition: String,
    @Json(name = "boot_count") val bootCount: Int,
    @Json(name = "restart_reason") val restartReason: String
)

@JsonClass(generateAdapter = true)
data class HealthResponse(
    val healthy: Boolean,
    val wifi: Boolean,
    @Json(name = "temperature_sensor") val temperatureSensor: Boolean,
    @Json(name = "led_strip") val ledStrip: Boolean,
    @Json(name = "led_schedule_enabled") val ledScheduleEnabled: Boolean,
    @Json(name = "temp_c") val tempC: Double,
    @Json(name = "free_heap") val freeHeap: Long,
    @Json(name = "min_free_heap") val minFreeHeap: Long,
    @Json(name = "uptime_s") val uptimeS: Long
)

@JsonClass(generateAdapter = true)
data class LedResponse(
    val on: Boolean,
    val brightness: Int,
    val r: Int,
    val g: Int,
    val b: Int,
    @Json(name = "num_leds") val numLeds: Int
)

@JsonClass(generateAdapter = true)
data class LedRequest(
    val on: Boolean? = null,
    val brightness: Int? = null,
    val r: Int? = null,
    val g: Int? = null,
    val b: Int? = null
)

@JsonClass(generateAdapter = true)
data class LedScheduleResponse(
    val enabled: Boolean,
    @Json(name = "on_hour") val onHour: Int,
    @Json(name = "on_minute") val onMinute: Int,
    @Json(name = "ramp_duration_min") val rampDurationMin: Int,
    @Json(name = "pause_enabled") val pauseEnabled: Boolean,
    @Json(name = "pause_start_hour") val pauseStartHour: Int,
    @Json(name = "pause_start_minute") val pauseStartMinute: Int,
    @Json(name = "pause_end_hour") val pauseEndHour: Int,
    @Json(name = "pause_end_minute") val pauseEndMinute: Int,
    @Json(name = "pause_brightness") val pauseBrightness: Int,
    @Json(name = "pause_red") val pauseRed: Int,
    @Json(name = "pause_green") val pauseGreen: Int,
    @Json(name = "pause_blue") val pauseBlue: Int,
    @Json(name = "off_hour") val offHour: Int,
    @Json(name = "off_minute") val offMinute: Int,
    val brightness: Int,
    val red: Int,
    val green: Int,
    val blue: Int
)

@JsonClass(generateAdapter = true)
data class LedScheduleRequest(
    val enabled: Boolean? = null,
    @Json(name = "on_hour") val onHour: Int? = null,
    @Json(name = "on_minute") val onMinute: Int? = null,
    @Json(name = "ramp_duration_min") val rampDurationMin: Int? = null,
    @Json(name = "pause_enabled") val pauseEnabled: Boolean? = null,
    @Json(name = "pause_start_hour") val pauseStartHour: Int? = null,
    @Json(name = "pause_start_minute") val pauseStartMinute: Int? = null,
    @Json(name = "pause_end_hour") val pauseEndHour: Int? = null,
    @Json(name = "pause_end_minute") val pauseEndMinute: Int? = null,
    @Json(name = "pause_brightness") val pauseBrightness: Int? = null,
    @Json(name = "pause_red") val pauseRed: Int? = null,
    @Json(name = "pause_green") val pauseGreen: Int? = null,
    @Json(name = "pause_blue") val pauseBlue: Int? = null,
    @Json(name = "off_hour") val offHour: Int? = null,
    @Json(name = "off_minute") val offMinute: Int? = null,
    val brightness: Int? = null,
    val red: Int? = null,
    val green: Int? = null,
    val blue: Int? = null
)

@JsonClass(generateAdapter = true)
data class PresetConfig(
    val enabled: Boolean? = null,
    @Json(name = "on_hour") val onHour: Int? = null,
    @Json(name = "on_minute") val onMinute: Int? = null,
    @Json(name = "ramp_duration_min") val rampDurationMin: Int? = null,
    @Json(name = "off_hour") val offHour: Int? = null,
    @Json(name = "off_minute") val offMinute: Int? = null,
    val brightness: Int? = null,
    val red: Int? = null,
    val green: Int? = null,
    val blue: Int? = null
)

@JsonClass(generateAdapter = true)
data class LedPreset(
    val slot: Int,
    val name: String,
    val config: PresetConfig
)

@JsonClass(generateAdapter = true)
data class LedPresetsResponse(val presets: List<LedPreset>)

@JsonClass(generateAdapter = true)
data class PresetActionRequest(
    val action: String,
    val slot: Int,
    val name: String? = null
)

@JsonClass(generateAdapter = true)
data class SceneResponse(
    val active: Int,
    @Json(name = "sunrise_duration_min") val sunriseDurationMin: Int,
    @Json(name = "sunrise_max_brightness") val sunriseMaxBrightness: Int,
    @Json(name = "sunset_duration_min") val sunsetDurationMin: Int,
    @Json(name = "moonlight_brightness") val moonlightBrightness: Int,
    @Json(name = "moonlight_r") val moonlightR: Int,
    @Json(name = "moonlight_g") val moonlightG: Int,
    @Json(name = "moonlight_b") val moonlightB: Int,
    @Json(name = "storm_intensity") val stormIntensity: Int,
    @Json(name = "clouds_depth") val cloudsDepth: Int,
    @Json(name = "clouds_period_s") val cloudsPeriodS: Int,
    @Json(name = "moon_phase") val moonPhase: Double
)

@JsonClass(generateAdapter = true)
data class SceneRequest(@Json(name = "start_scene") val startScene: Int)

@JsonClass(generateAdapter = true)
data class TemperatureResponse(val valid: Boolean, @Json(name = "temperature_c") val temperatureC: Double)

@JsonClass(generateAdapter = true)
data class TempSample(val t: Long, val c: Double)

@JsonClass(generateAdapter = true)
data class TemperatureHistoryResponse(
    val count: Int,
    @Json(name = "interval_sec") val intervalSec: Int,
    val samples: List<TempSample>
)

@JsonClass(generateAdapter = true)
data class RelaySchedule(
    val enabled: Boolean,
    @Json(name = "on_min") val onMin: Int,
    @Json(name = "off_min") val offMin: Int
)

@JsonClass(generateAdapter = true)
data class RelayInfo(
    val index: Int,
    val on: Boolean,
    val name: String,
    val schedules: List<RelaySchedule>
)

@JsonClass(generateAdapter = true)
data class RelaysResponse(val count: Int, val relays: List<RelayInfo>)

@JsonClass(generateAdapter = true)
data class RelayToggleRequest(val index: Int, val on: Boolean)

@JsonClass(generateAdapter = true)
data class RelayNameRequest(val index: Int, val name: String)

@JsonClass(generateAdapter = true)
data class HeaterResponse(
    val enabled: Boolean,
    @Json(name = "relay_index") val relayIndex: Int,
    @Json(name = "target_temp_c") val targetTempC: Double,
    @Json(name = "hysteresis_c") val hysteresisC: Double,
    @Json(name = "runaway_protection") val runawayProtection: Boolean,
    @Json(name = "runaway_timeout_min") val runawayTimeoutMin: Int
)

@JsonClass(generateAdapter = true)
data class HeaterRequest(
    val enabled: Boolean? = null,
    @Json(name = "relay_index") val relayIndex: Int? = null,
    @Json(name = "target_temp_c") val targetTempC: Double? = null,
    @Json(name = "hysteresis_c") val hysteresisC: Double? = null,
    @Json(name = "runaway_protection") val runawayProtection: Boolean? = null,
    @Json(name = "runaway_timeout_min") val runawayTimeoutMin: Int? = null
)

@JsonClass(generateAdapter = true)
data class Co2Response(
    val enabled: Boolean,
    @Json(name = "relay_index") val relayIndex: Int,
    @Json(name = "pre_on_min") val preOnMin: Int,
    @Json(name = "post_off_min") val postOffMin: Int
)

@JsonClass(generateAdapter = true)
data class Co2Request(
    val enabled: Boolean? = null,
    @Json(name = "relay_index") val relayIndex: Int? = null,
    @Json(name = "pre_on_min") val preOnMin: Int? = null,
    @Json(name = "post_off_min") val postOffMin: Int? = null
)

@JsonClass(generateAdapter = true)
data class FeedingResponse(
    val active: Boolean,
    @Json(name = "remaining_s") val remainingS: Int,
    @Json(name = "relay_index") val relayIndex: Int,
    @Json(name = "duration_min") val durationMin: Int,
    @Json(name = "dim_lights") val dimLights: Boolean,
    @Json(name = "dim_brightness") val dimBrightness: Int
)

@JsonClass(generateAdapter = true)
data class FeedingRequest(
    val action: String? = null,
    @Json(name = "relay_index") val relayIndex: Int? = null,
    @Json(name = "duration_min") val durationMin: Int? = null,
    @Json(name = "dim_lights") val dimLights: Boolean? = null,
    @Json(name = "dim_brightness") val dimBrightness: Int? = null
)

@JsonClass(generateAdapter = true)
data class TelegramResponse(
    @Json(name = "bot_token_set") val botTokenSet: Boolean,
    @Json(name = "chat_id") val chatId: String,
    val enabled: Boolean,
    @Json(name = "temp_alarm_enabled") val tempAlarmEnabled: Boolean,
    @Json(name = "temp_high_c") val tempHighC: Double,
    @Json(name = "temp_low_c") val tempLowC: Double,
    @Json(name = "water_change_enabled") val waterChangeEnabled: Boolean,
    @Json(name = "water_change_days") val waterChangeDays: Int,
    @Json(name = "fertilizer_enabled") val fertilizerEnabled: Boolean,
    @Json(name = "fertilizer_days") val fertilizerDays: Int,
    @Json(name = "daily_summary_enabled") val dailySummaryEnabled: Boolean,
    @Json(name = "daily_summary_hour") val dailySummaryHour: Int,
    @Json(name = "relay_notify_enabled") val relayNotifyEnabled: Boolean,
    @Json(name = "last_water_change") val lastWaterChange: Long,
    @Json(name = "last_fertilizer") val lastFertilizer: Long
)

@JsonClass(generateAdapter = true)
data class TelegramRequest(
    @Json(name = "bot_token") val botToken: String? = null,
    @Json(name = "chat_id") val chatId: String? = null,
    val enabled: Boolean? = null,
    @Json(name = "temp_alarm_enabled") val tempAlarmEnabled: Boolean? = null,
    @Json(name = "temp_high_c") val tempHighC: Double? = null,
    @Json(name = "temp_low_c") val tempLowC: Double? = null,
    @Json(name = "water_change_enabled") val waterChangeEnabled: Boolean? = null,
    @Json(name = "water_change_days") val waterChangeDays: Int? = null,
    @Json(name = "fertilizer_enabled") val fertilizerEnabled: Boolean? = null,
    @Json(name = "fertilizer_days") val fertilizerDays: Int? = null,
    @Json(name = "daily_summary_enabled") val dailySummaryEnabled: Boolean? = null,
    @Json(name = "daily_summary_hour") val dailySummaryHour: Int? = null,
    @Json(name = "relay_notify_enabled") val relayNotifyEnabled: Boolean? = null
)

@JsonClass(generateAdapter = true)
data class DuckDnsResponse(
    val domain: String,
    @Json(name = "token_set") val tokenSet: Boolean,
    val enabled: Boolean,
    @Json(name = "last_status") val lastStatus: String
)

@JsonClass(generateAdapter = true)
data class DuckDnsRequest(
    val domain: String? = null,
    val token: String? = null,
    val enabled: Boolean? = null
)

@JsonClass(generateAdapter = true)
data class TimezoneResponse(val tz: String)

@JsonClass(generateAdapter = true)
data class TimezoneRequest(val tz: String)

@JsonClass(generateAdapter = true)
data class DailyCycleResponse(
    val enabled: Boolean,
    val latitude: Double,
    val longitude: Double,
    val phase: Int,
    @Json(name = "sunrise_min") val sunriseMin: Int,
    @Json(name = "sunset_min") val sunsetMin: Int,
    @Json(name = "moonlight_duration_min") val moonlightDurationMin: Int
)

@JsonClass(generateAdapter = true)
data class DailyCycleRequest(
    val enabled: Boolean? = null,
    val latitude: Double? = null,
    val longitude: Double? = null,
    @Json(name = "moonlight_duration_min") val moonlightDurationMin: Int? = null
)

@JsonClass(generateAdapter = true)
data class EventItem(
    val id: Int,
    val type: String,
    val timestamp: Long,
    val message: String
)

@JsonClass(generateAdapter = true)
data class EventsResponse(val count: Int, val events: List<EventItem>)

@JsonClass(generateAdapter = true)
data class OtaRequest(val url: String)

@JsonClass(generateAdapter = true)
data class OtaStatusResponse(val status: String, val progress: Int, val error: String)

@JsonClass(generateAdapter = true)
data class MdnsResponse(val hostname: String, val enabled: Boolean)

@JsonClass(generateAdapter = true)
data class MdnsRequest(val hostname: String? = null, val enabled: Boolean? = null)

@JsonClass(generateAdapter = true)
data class OkResponse(val ok: Boolean)

data class WsStatus(
    val tempC: Double = 0.0,
    val tempOk: Boolean = true,
    val uptimeS: Long = 0,
    val freeHeap: Long = 0,
    val phase: Int = 0,
    val connected: Boolean = false
)
