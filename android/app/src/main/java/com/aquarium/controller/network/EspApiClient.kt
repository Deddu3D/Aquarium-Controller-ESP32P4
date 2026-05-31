package com.aquarium.controller.network

import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * HTTP client for talking to the ESP after provisioning (via DuckDNS URL).
 *
 * Self-signed certificates are accepted intentionally: the ESP32 uses a
 * self-signed cert and there is no way to install it as a trusted CA
 * automatically. The same policy is applied in WebViewScreen.
 */
object EspApiClient {

    private val trustAllCerts: Array<TrustManager> = arrayOf(
        object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) = Unit
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
    )

    private val client: OkHttpClient by lazy {
        val sslContext = SSLContext.getInstance("TLS").apply {
            init(null, trustAllCerts, SecureRandom())
        }
        OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(10, TimeUnit.SECONDS)
            .sslSocketFactory(sslContext.socketFactory, trustAllCerts[0] as X509TrustManager)
            .hostnameVerifier { _, _ -> true }
            .build()
    }

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
