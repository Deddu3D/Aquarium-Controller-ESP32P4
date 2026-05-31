package com.aquarium.controller.data

import android.content.Context
import androidx.core.content.edit

/**
 * Simple SharedPreferences wrapper that persists the DuckDNS URL
 * and port used to reach the ESP after initial setup.
 */
class PrefsRepository(context: Context) {

    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /** Full URL to the ESP web UI, e.g. "http://mioaqua.duckdns.org" or "https://mioaqua.duckdns.org" */
    var espUrl: String?
        get() = prefs.getString(KEY_ESP_URL, null)
        set(value) = prefs.edit { putString(KEY_ESP_URL, value) }

    /** Whether the first-run wizard has been completed. */
    var setupComplete: Boolean
        get() = prefs.getBoolean(KEY_SETUP_COMPLETE, false)
        set(value) = prefs.edit { putBoolean(KEY_SETUP_COMPLETE, value) }

    /** Build a full URL from domain and port. Port 80 → http://, port 443 → https://, other → http://:port */
    companion object {
        private const val PREFS_NAME = "aquarium_prefs"
        private const val KEY_ESP_URL = "esp_url"
        private const val KEY_SETUP_COMPLETE = "setup_complete"

        fun buildUrl(domain: String, port: Int): String {
            val cleanDomain = domain.trim()
            val fullDomain = if (cleanDomain.contains(".")) cleanDomain
                             else "$cleanDomain.duckdns.org"
            return when (port) {
                80  -> "http://$fullDomain"
                443 -> "https://$fullDomain"
                else -> "http://$fullDomain:$port"
            }
        }
    }
}
