package com.aquarium.controller.repository

import com.aquarium.controller.data.api.AquariumApi
import com.aquarium.controller.data.model.*
import com.aquarium.controller.data.network.MqttRemoteManager
import com.aquarium.controller.data.network.SessionCookieJar
import com.aquarium.controller.data.network.WebSocketManager
import com.aquarium.controller.data.prefs.ConnectionPreferences
import com.aquarium.controller.data.prefs.ConnectionSettings
import com.squareup.moshi.Moshi
import dagger.hilt.android.scopes.ActivityRetainedScoped
import kotlinx.coroutines.flow.first
import okhttp3.OkHttpClient
import okhttp3.RequestBody.Companion.toRequestBody
import retrofit2.Response
import retrofit2.Retrofit
import retrofit2.converter.moshi.MoshiConverterFactory
import javax.inject.Inject

@ActivityRetainedScoped
class AquariumRepository @Inject constructor(
    private val retrofit: Retrofit,
    private val okHttpClient: OkHttpClient,
    private val moshi: Moshi,
    private val cookieJar: SessionCookieJar,
    private val connectionPrefs: ConnectionPreferences,
    val webSocketManager: WebSocketManager,
    val mqttRemoteManager: MqttRemoteManager
) {
    private var api: AquariumApi = retrofit.create(AquariumApi::class.java)

    private fun buildApiForUrl(baseUrl: String): AquariumApi {
        return retrofit.newBuilder()
            .baseUrl(baseUrl)
            .build()
            .create(AquariumApi::class.java)
    }

    suspend fun setBaseUrl(baseUrl: String) {
        api = buildApiForUrl(baseUrl)
    }

    suspend fun initFromPrefs() {
        val settings = connectionPrefs.settings.first()
        if (settings.host.isNotBlank()) {
            api = buildApiForUrl(settings.baseUrl)
        }
        // Reconnect MQTT if it was enabled
        if (settings.mqttEnabled && settings.deviceId.isNotBlank()) {
            mqttRemoteManager.connect(settings.deviceId)
        }
    }

    suspend fun saveConnectionSettings(settings: ConnectionSettings) {
        connectionPrefs.saveSettings(settings)
        api = buildApiForUrl(settings.baseUrl)
        if (settings.mqttEnabled && settings.deviceId.isNotBlank()) {
            mqttRemoteManager.connect(settings.deviceId)
        } else if (!settings.mqttEnabled) {
            mqttRemoteManager.disconnect()
        }
    }

    fun connectWebSocket(baseUrl: String) = webSocketManager.connect(baseUrl)
    fun disconnectWebSocket() = webSocketManager.disconnect()

    // ── Remote MQTT helpers ───────────────────────────────────────────

    /** Connect the MQTT manager to the given device ID and persist the setting. */
    suspend fun enableRemoteAccess(deviceId: String) {
        connectionPrefs.saveDeviceId(deviceId)
        connectionPrefs.saveMqttEnabled(true)
        mqttRemoteManager.connect(deviceId)
    }

    /** Disconnect MQTT and persist the disabled state. */
    suspend fun disableRemoteAccess() {
        connectionPrefs.saveMqttEnabled(false)
        mqttRemoteManager.disconnect()
    }

    suspend fun login(username: String, password: String): Result<LoginResponse> =
        safeCall { api.login(LoginRequest(username, password)) }

    suspend fun logout(): Result<OkResponse> {
        val result = safeCall { api.logout() }
        cookieJar.clearSession()
        webSocketManager.disconnect()
        return result
    }

    suspend fun changeAuth(username: String, password: String): Result<OkResponse> =
        safeCall { api.changeAuth(AuthChangeRequest(username, password)) }

    suspend fun getStatus(): Result<StatusResponse> = safeCall { api.getStatus() }
    suspend fun getHealth(): Result<HealthResponse> = safeCall { api.getHealth() }

    suspend fun getLeds(): Result<LedResponse> = safeCall { api.getLeds() }
    suspend fun postLeds(request: LedRequest): Result<OkResponse> = safeCall { api.postLeds(request) }

    suspend fun getLedSchedule(): Result<LedScheduleResponse> = safeCall { api.getLedSchedule() }
    suspend fun postLedSchedule(request: LedScheduleRequest): Result<OkResponse> = safeCall { api.postLedSchedule(request) }

    suspend fun getLedPresets(): Result<LedPresetsResponse> = safeCall { api.getLedPresets() }
    suspend fun postLedPresets(request: PresetActionRequest): Result<OkResponse> = safeCall { api.postLedPresets(request) }

    suspend fun getScene(): Result<SceneResponse> = safeCall { api.getScene() }
    suspend fun postScene(sceneIndex: Int): Result<OkResponse> = safeCall { api.postScene(SceneRequest(sceneIndex)) }

    suspend fun getTemperature(): Result<TemperatureResponse> = safeCall { api.getTemperature() }
    suspend fun getTemperatureHistory(): Result<TemperatureHistoryResponse> = safeCall { api.getTemperatureHistory() }
    suspend fun exportTemperatureCsv(): Result<okhttp3.ResponseBody> = safeCallRaw { api.exportTemperatureCsv() }

    suspend fun getRelays(): Result<RelaysResponse> = safeCall { api.getRelays() }
    suspend fun toggleRelay(index: Int, on: Boolean): Result<OkResponse> = safeCall { api.postRelayToggle(RelayToggleRequest(index, on)) }
    suspend fun renameRelay(index: Int, name: String): Result<OkResponse> = safeCall { api.postRelayName(RelayNameRequest(index, name)) }

    suspend fun getHeater(): Result<HeaterResponse> = safeCall { api.getHeater() }
    suspend fun postHeater(request: HeaterRequest): Result<OkResponse> = safeCall { api.postHeater(request) }

    suspend fun getCo2(): Result<Co2Response> = safeCall { api.getCo2() }
    suspend fun postCo2(request: Co2Request): Result<OkResponse> = safeCall { api.postCo2(request) }

    suspend fun getFeeding(): Result<FeedingResponse> = safeCall { api.getFeeding() }
    suspend fun postFeeding(request: FeedingRequest): Result<OkResponse> = safeCall { api.postFeeding(request) }

    suspend fun getTelegram(): Result<TelegramResponse> = safeCall { api.getTelegram() }
    suspend fun postTelegram(request: TelegramRequest): Result<OkResponse> = safeCall { api.postTelegram(request) }
    suspend fun telegramTest(): Result<OkResponse> = safeCall { api.telegramTest() }
    suspend fun telegramWaterChange(): Result<OkResponse> = safeCall { api.telegramWaterChange() }
    suspend fun telegramFertilizer(): Result<OkResponse> = safeCall { api.telegramFertilizer() }

    suspend fun getDuckDns(): Result<DuckDnsResponse> = safeCall { api.getDuckDns() }
    suspend fun postDuckDns(request: DuckDnsRequest): Result<OkResponse> = safeCall { api.postDuckDns(request) }
    suspend fun duckDnsUpdate(): Result<OkResponse> = safeCall { api.duckDnsUpdate() }

    suspend fun getTimezone(): Result<TimezoneResponse> = safeCall { api.getTimezone() }
    suspend fun postTimezone(tz: String): Result<OkResponse> = safeCall { api.postTimezone(TimezoneRequest(tz)) }

    suspend fun getDailyCycle(): Result<DailyCycleResponse> = safeCall { api.getDailyCycle() }
    suspend fun postDailyCycle(request: DailyCycleRequest): Result<OkResponse> = safeCall { api.postDailyCycle(request) }

    suspend fun getEvents(): Result<EventsResponse> = safeCall { api.getEvents() }

    suspend fun postOta(url: String): Result<OkResponse> = safeCall { api.postOta(OtaRequest(url)) }
    suspend fun getOtaStatus(): Result<OtaStatusResponse> = safeCall { api.getOtaStatus() }

    suspend fun getMdns(): Result<MdnsResponse> = safeCall { api.getMdns() }
    suspend fun postMdns(request: MdnsRequest): Result<OkResponse> = safeCall { api.postMdns(request) }

    suspend fun getRemote(): Result<RemoteResponse> = safeCall { api.getRemote() }

    suspend fun factoryReset(): Result<OkResponse> = safeCall { api.factoryReset() }
    suspend fun exportConfig(): Result<okhttp3.ResponseBody> = safeCallRaw { api.exportConfig() }
    suspend fun importConfig(json: String): Result<OkResponse> = safeCall {
        api.importConfig(json.toRequestBody())
    }

    private suspend fun <T> safeCall(call: suspend () -> Response<T>): Result<T> {
        return try {
            val response = call()
            if (response.isSuccessful) {
                val body = response.body()
                if (body != null) Result.success(body)
                else Result.failure(Exception("Empty response body"))
            } else {
                Result.failure(Exception("HTTP ${response.code()}: ${response.message()}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    private suspend fun safeCallRaw(call: suspend () -> Response<okhttp3.ResponseBody>): Result<okhttp3.ResponseBody> {
        return try {
            val response = call()
            if (response.isSuccessful) {
                val body = response.body()
                if (body != null) Result.success(body)
                else Result.failure(Exception("Empty response body"))
            } else {
                Result.failure(Exception("HTTP ${response.code()}: ${response.message()}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
}
