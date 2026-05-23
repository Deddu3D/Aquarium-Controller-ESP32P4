package com.aquarium.controller.data.prefs

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.aquarium.controller.data.model.EspDevice
import com.squareup.moshi.JsonClass
import com.squareup.moshi.Moshi
import com.squareup.moshi.Types
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

val Context.dataStore: DataStore<Preferences> by preferencesDataStore(name = "connection_prefs")

@Singleton
class ConnectionPreferences @Inject constructor(
    @ApplicationContext private val context: Context,
    private val moshi: Moshi
) {
    companion object {
        private val KEY_GOOGLE_USER_ID = stringPreferencesKey("google_user_id")
        private val KEY_GOOGLE_EMAIL = stringPreferencesKey("google_email")
        private val KEY_GOOGLE_DISPLAY_NAME = stringPreferencesKey("google_display_name")
        private val KEY_CURRENT_DEVICE_ID = stringPreferencesKey("current_device_id")
        private val KEY_DEVICES_JSON = stringPreferencesKey("devices_json")
    }

    // ── Google user ────────────────────────────────────────────────────

    val googleUserId: Flow<String?> = context.dataStore.data.map { it[KEY_GOOGLE_USER_ID] }
    val googleEmail: Flow<String?> = context.dataStore.data.map { it[KEY_GOOGLE_EMAIL] }
    val googleDisplayName: Flow<String?> = context.dataStore.data.map { it[KEY_GOOGLE_DISPLAY_NAME] }

    suspend fun saveGoogleUser(userId: String, email: String, displayName: String) {
        context.dataStore.edit { prefs ->
            prefs[KEY_GOOGLE_USER_ID] = userId
            prefs[KEY_GOOGLE_EMAIL] = email
            prefs[KEY_GOOGLE_DISPLAY_NAME] = displayName
        }
    }

    suspend fun clearGoogleUser() {
        context.dataStore.edit { prefs ->
            prefs.remove(KEY_GOOGLE_USER_ID)
            prefs.remove(KEY_GOOGLE_EMAIL)
            prefs.remove(KEY_GOOGLE_DISPLAY_NAME)
        }
    }

    // ── Device list ────────────────────────────────────────────────────

    val devices: Flow<List<EspDevice>> = context.dataStore.data.map { prefs ->
        prefs[KEY_DEVICES_JSON]?.let { deserializeDevices(it) } ?: emptyList()
    }

    val currentDeviceId: Flow<String?> = context.dataStore.data.map { it[KEY_CURRENT_DEVICE_ID] }

    val currentDevice: Flow<EspDevice?> = context.dataStore.data.map { prefs ->
        val list = prefs[KEY_DEVICES_JSON]?.let { deserializeDevices(it) } ?: return@map null
        val id = prefs[KEY_CURRENT_DEVICE_ID]
        list.firstOrNull { it.id == id } ?: list.firstOrNull()
    }

    suspend fun addDevice(device: EspDevice) {
        context.dataStore.edit { prefs ->
            val current = prefs[KEY_DEVICES_JSON]?.let { deserializeDevices(it) } ?: emptyList()
            val updated = current + device
            prefs[KEY_DEVICES_JSON] = serializeDevices(updated)
            // Auto-select newly added device if none selected
            if (prefs[KEY_CURRENT_DEVICE_ID] == null) {
                prefs[KEY_CURRENT_DEVICE_ID] = device.id
            }
        }
    }

    suspend fun removeDevice(deviceId: String) {
        context.dataStore.edit { prefs ->
            val current = prefs[KEY_DEVICES_JSON]?.let { deserializeDevices(it) } ?: emptyList()
            val updated = current.filter { it.id != deviceId }
            prefs[KEY_DEVICES_JSON] = serializeDevices(updated)
            if (prefs[KEY_CURRENT_DEVICE_ID] == deviceId) {
                prefs[KEY_CURRENT_DEVICE_ID] = updated.firstOrNull()?.id ?: ""
            }
        }
    }

    suspend fun selectDevice(deviceId: String) {
        context.dataStore.edit { prefs ->
            prefs[KEY_CURRENT_DEVICE_ID] = deviceId
        }
    }

    // ── Serialization helpers ──────────────────────────────────────────

    @JsonClass(generateAdapter = true)
    data class EspDeviceJson(val id: String, val name: String, val duckDnsDomain: String)

    private fun serializeDevices(devices: List<EspDevice>): String {
        val adapter = moshi.adapter<List<EspDeviceJson>>(
            Types.newParameterizedType(List::class.java, EspDeviceJson::class.java)
        )
        return adapter.toJson(devices.map { EspDeviceJson(it.id, it.name, it.duckDnsDomain) })
    }

    private fun deserializeDevices(json: String): List<EspDevice> {
        return try {
            val adapter = moshi.adapter<List<EspDeviceJson>>(
                Types.newParameterizedType(List::class.java, EspDeviceJson::class.java)
            )
            adapter.fromJson(json)?.map { EspDevice(it.id, it.name, it.duckDnsDomain) } ?: emptyList()
        } catch (e: Exception) {
            emptyList()
        }
    }
}
