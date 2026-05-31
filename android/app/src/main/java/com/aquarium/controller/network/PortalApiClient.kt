package com.aquarium.controller.network

import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.util.concurrent.TimeUnit

private const val PORTAL_BASE = "http://192.168.4.1"
private val JSON_MT = "application/json; charset=utf-8".toMediaType()

/**
 * HTTP client for talking to the ESP while it is in AP / provisioning mode.
 * All requests go to http://192.168.4.1 (plain HTTP, allowed by network_security_config.xml).
 */
object PortalApiClient {

    private val client = OkHttpClient.Builder()
        .connectTimeout(8, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .build()

    /** Ping the portal to verify connectivity. Returns true if the ESP is reachable. */
    fun ping(): Boolean {
        return try {
            val req = Request.Builder().url("$PORTAL_BASE/api/scan").build()
            client.newCall(req).execute().use { it.isSuccessful }
        } catch (_: IOException) {
            false
        }
    }

    /**
     * Fetch available WiFi networks from the ESP scanner.
     * Returns a JSON string like {"networks":[{"ssid":"…","rssi":-55,"open":false},…]}
     * or null on error.
     */
    fun scanNetworks(): String? {
        return try {
            val req = Request.Builder().url("$PORTAL_BASE/api/scan").build()
            client.newCall(req).execute().use { resp ->
                if (resp.isSuccessful) resp.body?.string() else null
            }
        } catch (_: IOException) {
            null
        }
    }

    /**
     * Send the full provisioning payload to the ESP.
     * Returns true if the ESP accepted the request (HTTP 200 + {"status":"ok"}).
     */
    fun provision(payload: String): Boolean {
        return try {
            val body = payload.toRequestBody(JSON_MT)
            val req = Request.Builder()
                .url("$PORTAL_BASE/api/provision")
                .post(body)
                .build()
            client.newCall(req).execute().use { resp ->
                if (!resp.isSuccessful) return false
                val text = resp.body?.string() ?: return false
                text.contains("\"ok\"")
            }
        } catch (_: IOException) {
            false
        }
    }
}
