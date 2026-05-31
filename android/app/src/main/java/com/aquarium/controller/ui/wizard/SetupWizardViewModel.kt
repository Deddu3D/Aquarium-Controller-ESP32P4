package com.aquarium.controller.ui.wizard

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.PrefsRepository
import com.aquarium.controller.network.EspApiClient
import com.aquarium.controller.network.PortalApiClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

class SetupWizardViewModel(application: Application) : AndroidViewModel(application) {

    private val prefs = PrefsRepository(application)

    private val _state = MutableStateFlow(SetupWizardState())
    val state: StateFlow<SetupWizardState> = _state

    // ── Navigation ─────────────────────────────────────────────────────────────

    fun nextStep() {
        val s = _state.value
        if (s.currentStep < WIZARD_TOTAL_STEPS - 1)
            _state.value = s.copy(currentStep = s.currentStep + 1, error = null)
    }

    fun prevStep() {
        val s = _state.value
        if (s.currentStep > 0)
            _state.value = s.copy(currentStep = s.currentStep - 1, error = null)
    }

    // ── Field updates ──────────────────────────────────────────────────────────

    fun onSsidChange(v: String) { _state.value = _state.value.copy(selectedSsid = v) }
    fun onPasswordChange(v: String) { _state.value = _state.value.copy(wifiPassword = v) }
    fun onTelegramTokenChange(v: String) { _state.value = _state.value.copy(telegramToken = v) }
    fun onTelegramChatIdChange(v: String) { _state.value = _state.value.copy(telegramChatId = v) }
    fun onDuckdnsDomainChange(v: String) { _state.value = _state.value.copy(duckdnsDomain = v) }
    fun onDuckdnsTokenChange(v: String) { _state.value = _state.value.copy(duckdnsToken = v) }
    fun onLanPortChange(v: String) { _state.value = _state.value.copy(lanPort = v) }
    fun onAquariumTypeChange(v: AquariumType) { _state.value = _state.value.copy(aquariumType = v) }

    // ── Step 0: check AP connectivity ─────────────────────────────────────────

    fun checkConnection() {
        _state.value = _state.value.copy(isCheckingConnection = true, connectionOk = false, error = null)
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { PortalApiClient.ping() }
            _state.value = _state.value.copy(
                isCheckingConnection = false,
                connectionOk = ok,
                error = if (!ok) "ESP non raggiungibile" else null,
            )
        }
    }

    // ── Step 1: WiFi scan ──────────────────────────────────────────────────────

    fun scanNetworks() {
        _state.value = _state.value.copy(isScanning = true, scanError = false)
        viewModelScope.launch {
            val json = withContext(Dispatchers.IO) { PortalApiClient.scanNetworks() }
            val networks = parseNetworks(json)
            _state.value = _state.value.copy(
                isScanning = false,
                networks = networks,
                scanError = networks.isEmpty() && json == null,
            )
        }
    }

    private fun parseNetworks(json: String?): List<WifiNetwork> {
        if (json == null) return emptyList()
        return try {
            val arr: JSONArray = JSONObject(json).getJSONArray("networks")
            (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                WifiNetwork(
                    ssid = o.optString("ssid", ""),
                    rssi = o.optInt("rssi", -100),
                    open = o.optBoolean("open", false),
                )
            }.sortedByDescending { it.rssi }
        } catch (_: Exception) {
            emptyList()
        }
    }

    // ── Step 5: send provision + poll ─────────────────────────────────────────

    fun sendProvision(onSuccess: (String) -> Unit) {
        val s = _state.value
        val port = s.lanPort.toIntOrNull() ?: 443

        _state.value = s.copy(isSending = true, error = null)
        viewModelScope.launch {
            val payload = buildPayload(s, port)
            val ok = withContext(Dispatchers.IO) { PortalApiClient.provision(payload) }
            if (!ok) {
                _state.value = _state.value.copy(isSending = false, error = "Errore di comunicazione con l'ESP")
                return@launch
            }
            // Provision accepted – ESP will reboot.
            _state.value = _state.value.copy(
                isSending = false,
                configSent = true,
                waitingForReconnect = true,
            )
        }
    }

    fun startPolling(onSuccess: (String) -> Unit) {
        val s = _state.value
        val port = s.lanPort.toIntOrNull() ?: 443
        val baseUrl = PrefsRepository.buildUrl(s.duckdnsDomain, port)

        _state.value = _state.value.copy(waitingForReconnect = false, pollingAttempt = 0)

        viewModelScope.launch {
            repeat(12) { attempt ->
                _state.value = _state.value.copy(pollingAttempt = attempt + 1)
                val ok = withContext(Dispatchers.IO) { EspApiClient.ping(baseUrl) }
                if (ok) {
                    // Save URL to prefs
                    prefs.espUrl = baseUrl
                    prefs.setupComplete = true
                    _state.value = _state.value.copy(pollingSuccess = true)
                    onSuccess(baseUrl)
                    return@launch
                }
                delay(5_000)
            }
            // Timeout
            _state.value = _state.value.copy(pollingFailed = true)
        }
    }

    private fun buildPayload(s: SetupWizardState, port: Int): String {
        return JSONObject().apply {
            put("ssid", s.selectedSsid)
            put("password", s.wifiPassword)
            if (s.telegramToken.isNotBlank()) {
                put("telegram_token", s.telegramToken)
                put("telegram_chat_id", s.telegramChatId)
            }
            if (s.duckdnsDomain.isNotBlank()) {
                put("duckdns_domain", s.duckdnsDomain.trim())
                put("duckdns_token", s.duckdnsToken.trim())
                put("lan_port", port)
            }
            put("aquarium_type", s.aquariumType.apiValue)
        }.toString()
    }
}
