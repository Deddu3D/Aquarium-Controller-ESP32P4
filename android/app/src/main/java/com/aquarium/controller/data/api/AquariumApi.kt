package com.aquarium.controller.data.api

import com.aquarium.controller.data.model.*
import okhttp3.ResponseBody
import retrofit2.Response
import retrofit2.http.*

interface AquariumApi {

    @POST("api/login")
    suspend fun login(@Body request: LoginRequest): Response<LoginResponse>

    @POST("api/logout")
    suspend fun logout(): Response<OkResponse>

    @POST("api/auth")
    suspend fun changeAuth(@Body request: AuthChangeRequest): Response<OkResponse>

    @GET("api/status")
    suspend fun getStatus(): Response<StatusResponse>

    @GET("api/health")
    suspend fun getHealth(): Response<HealthResponse>

    @GET("api/leds")
    suspend fun getLeds(): Response<LedResponse>

    @POST("api/leds")
    suspend fun postLeds(@Body request: LedRequest): Response<OkResponse>

    @GET("api/led_schedule")
    suspend fun getLedSchedule(): Response<LedScheduleResponse>

    @POST("api/led_schedule")
    suspend fun postLedSchedule(@Body request: LedScheduleRequest): Response<OkResponse>

    @GET("api/led_presets")
    suspend fun getLedPresets(): Response<LedPresetsResponse>

    @POST("api/led_presets")
    suspend fun postLedPresets(@Body request: PresetActionRequest): Response<OkResponse>

    @GET("api/scene")
    suspend fun getScene(): Response<SceneResponse>

    @POST("api/scene")
    suspend fun postScene(@Body request: SceneRequest): Response<OkResponse>

    @GET("api/temperature")
    suspend fun getTemperature(): Response<TemperatureResponse>

    @GET("api/temperature_history")
    suspend fun getTemperatureHistory(): Response<TemperatureHistoryResponse>

    @GET("api/temperature/export.csv")
    suspend fun exportTemperatureCsv(): Response<ResponseBody>

    @GET("api/relays")
    suspend fun getRelays(): Response<RelaysResponse>

    @POST("api/relays")
    suspend fun postRelayToggle(@Body request: RelayToggleRequest): Response<OkResponse>

    @POST("api/relays")
    suspend fun postRelayName(@Body request: RelayNameRequest): Response<OkResponse>

    @GET("api/heater")
    suspend fun getHeater(): Response<HeaterResponse>

    @POST("api/heater")
    suspend fun postHeater(@Body request: HeaterRequest): Response<OkResponse>

    @GET("api/co2")
    suspend fun getCo2(): Response<Co2Response>

    @POST("api/co2")
    suspend fun postCo2(@Body request: Co2Request): Response<OkResponse>

    @GET("api/feeding")
    suspend fun getFeeding(): Response<FeedingResponse>

    @POST("api/feeding")
    suspend fun postFeeding(@Body request: FeedingRequest): Response<OkResponse>

    @GET("api/telegram")
    suspend fun getTelegram(): Response<TelegramResponse>

    @POST("api/telegram")
    suspend fun postTelegram(@Body request: TelegramRequest): Response<OkResponse>

    @POST("api/telegram_test")
    suspend fun telegramTest(): Response<OkResponse>

    @POST("api/telegram_wc")
    suspend fun telegramWaterChange(): Response<OkResponse>

    @POST("api/telegram_fert")
    suspend fun telegramFertilizer(): Response<OkResponse>

    @GET("api/duckdns")
    suspend fun getDuckDns(): Response<DuckDnsResponse>

    @POST("api/duckdns")
    suspend fun postDuckDns(@Body request: DuckDnsRequest): Response<OkResponse>

    @POST("api/duckdns_update")
    suspend fun duckDnsUpdate(): Response<OkResponse>

    @GET("api/timezone")
    suspend fun getTimezone(): Response<TimezoneResponse>

    @POST("api/timezone")
    suspend fun postTimezone(@Body request: TimezoneRequest): Response<OkResponse>

    @GET("api/daily_cycle")
    suspend fun getDailyCycle(): Response<DailyCycleResponse>

    @POST("api/daily_cycle")
    suspend fun postDailyCycle(@Body request: DailyCycleRequest): Response<OkResponse>

    @GET("api/events")
    suspend fun getEvents(): Response<EventsResponse>

    @POST("api/ota")
    suspend fun postOta(@Body request: OtaRequest): Response<OkResponse>

    @GET("api/ota_status")
    suspend fun getOtaStatus(): Response<OtaStatusResponse>

    @GET("api/mdns")
    suspend fun getMdns(): Response<MdnsResponse>

    @POST("api/mdns")
    suspend fun postMdns(@Body request: MdnsRequest): Response<OkResponse>

    @GET("api/remote")
    suspend fun getRemote(): Response<RemoteResponse>

    @POST("api/factory_reset")
    suspend fun factoryReset(): Response<OkResponse>

    @GET("api/config/export")
    suspend fun exportConfig(): Response<ResponseBody>

    @POST("api/config/import")
    @Multipart
    suspend fun importConfig(@Part("config") config: okhttp3.RequestBody): Response<OkResponse>
}
