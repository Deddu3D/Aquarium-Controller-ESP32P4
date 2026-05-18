package com.aquarium.controller.data.prefs

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

val Context.dataStore: DataStore<Preferences> by preferencesDataStore(name = "connection_prefs")

data class ConnectionSettings(
    val host: String = "",
    val port: Int = 443,
    val useHttps: Boolean = true,
    val username: String = "admin",
    /** 12-char hex device ID derived from MAC, populated during provisioning. */
    val deviceId: String = "",
    /** Whether zero-config MQTT remote access is enabled. */
    val mqttEnabled: Boolean = false
) {
    val baseUrl: String
        get() {
            val scheme = if (useHttps) "https" else "http"
            return "$scheme://$host:$port/"
        }
}

@Singleton
class ConnectionPreferences @Inject constructor(
    @ApplicationContext private val context: Context
) {
    companion object {
        private val KEY_HOST         = stringPreferencesKey("host")
        private val KEY_PORT         = intPreferencesKey("port")
        private val KEY_USE_HTTPS    = booleanPreferencesKey("use_https")
        private val KEY_USERNAME     = stringPreferencesKey("username")
        private val KEY_DEVICE_ID    = stringPreferencesKey("device_id")
        private val KEY_MQTT_ENABLED = booleanPreferencesKey("mqtt_enabled")
    }

    val settings: Flow<ConnectionSettings> = context.dataStore.data.map { prefs ->
        ConnectionSettings(
            host        = prefs[KEY_HOST]         ?: "",
            port        = prefs[KEY_PORT]         ?: 443,
            useHttps    = prefs[KEY_USE_HTTPS]    ?: true,
            username    = prefs[KEY_USERNAME]     ?: "admin",
            deviceId    = prefs[KEY_DEVICE_ID]    ?: "",
            mqttEnabled = prefs[KEY_MQTT_ENABLED] ?: false
        )
    }

    suspend fun saveSettings(settings: ConnectionSettings) {
        context.dataStore.edit { prefs ->
            prefs[KEY_HOST]         = settings.host
            prefs[KEY_PORT]         = settings.port
            prefs[KEY_USE_HTTPS]    = settings.useHttps
            prefs[KEY_USERNAME]     = settings.username
            prefs[KEY_DEVICE_ID]    = settings.deviceId
            prefs[KEY_MQTT_ENABLED] = settings.mqttEnabled
        }
    }

    suspend fun saveUsername(username: String) {
        context.dataStore.edit { prefs ->
            prefs[KEY_USERNAME] = username
        }
    }

    suspend fun saveDeviceId(deviceId: String) {
        context.dataStore.edit { prefs ->
            prefs[KEY_DEVICE_ID] = deviceId
        }
    }

    suspend fun saveMqttEnabled(enabled: Boolean) {
        context.dataStore.edit { prefs ->
            prefs[KEY_MQTT_ENABLED] = enabled
        }
    }
}
