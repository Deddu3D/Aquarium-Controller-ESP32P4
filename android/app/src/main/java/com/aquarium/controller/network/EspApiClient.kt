package com.aquarium.controller.network

import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * HTTP client for talking to the ESP after provisioning (via DuckDNS URL).
 */
object EspApiClient {

    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .build()

    /**
     * Ping the ESP at [baseUrl]/api/ping.
     * Returns true if the ESP responds with HTTP 200.
     */
    fun ping(baseUrl: String): Boolean {
        return try {
            val req = Request.Builder().url("$baseUrl/api/ping").build()
            client.newCall(req).execute().use { it.isSuccessful }
        } catch (_: IOException) {
            false
        }
    }
}
