package com.aquarium.controller.network

import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

/**
 * HTTPS client for talking to the ESP after provisioning (via DuckDNS URL).
 * Accepts self-signed certificates because the ESP uses a self-signed TLS cert.
 *
 * SECURITY NOTE: This trust-all approach is acceptable for a local home device
 * on a trusted LAN/DuckDNS tunnel. For production use, pin the ESP certificate.
 */
object EspApiClient {

    private val trustAll = object : X509TrustManager {
        override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
        override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) = Unit
        override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
    }

    private val sslContext = SSLContext.getInstance("TLS").apply {
        init(null, arrayOf(trustAll), null)
    }

    private val client = OkHttpClient.Builder()
        .sslSocketFactory(sslContext.socketFactory, trustAll)
        .hostnameVerifier { _, _ -> true }
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
