package com.aquarium.controller.ui.provision

import android.content.Context
import android.content.Intent
import android.provider.Settings
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.model.DuckDnsRequest
import com.aquarium.controller.data.model.TelegramRequest
import com.aquarium.controller.data.model.WifiNetworkInfo
import com.aquarium.controller.data.model.WifiScanResponse
import com.aquarium.controller.data.prefs.ConnectionPreferences
import com.aquarium.controller.data.prefs.ConnectionSettings
import com.aquarium.controller.repository.AquariumRepository
import com.squareup.moshi.Moshi
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.util.concurrent.TimeUnit
import javax.inject.Inject

enum class ProvisionStep {
    CONNECT_TO_AP,  // Step 1: connect phone to AquariumSetup
    PICK_WIFI,      // Step 2: choose home network + enter password
    APPLYING,       // Step 3: sending credentials (auto-advance)
    RECONNECT,      // Step 4: reconnect phone to home WiFi + verify
    SERVICES,       // Step 5: optional Telegram + DuckDNS quick-setup
    COMPLETE        // wizard finished, navigate to Login
}

data class ProvisionUiState(
    val step: ProvisionStep = ProvisionStep.CONNECT_TO_AP,
    val networks: List<WifiNetworkInfo> = emptyList(),
    val isScanning: Boolean = false,
    val selectedSsid: String = "",
    val password: String = "",
    val mdnsHostname: String = "aquarium",
    val isLoading: Boolean = false,
    val error: String? = null,
    // Services step
    val telegramToken: String = "",
    val telegramChatId: String = "",
    val duckDnsDomain: String = "",
    val duckDnsToken: String = "",
    val servicesSaving: Boolean = false,
    val navigateToLogin: Boolean = false
)

@HiltViewModel
class ProvisionViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val connectionPrefs: ConnectionPreferences,
    private val repository: AquariumRepository,
    private val moshi: Moshi
) : ViewModel() {

    companion object {
        private const val AP_BASE = "http://192.168.4.1"
    }

    private val _uiState = MutableStateFlow(ProvisionUiState())
    val uiState: StateFlow<ProvisionUiState> = _uiState.asStateFlow()

    /** Plain HTTP client for the provisioning AP (no TLS, no auth). */
    private val apClient = OkHttpClient.Builder()
        .connectTimeout(8, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS) // scan can take ~3 s on the ESP
        .writeTimeout(8, TimeUnit.SECONDS)
        .build()

    // ── Step navigation helpers ────────────────────────────────────────

    /** Step 1 → Step 2: advance and immediately trigger a scan. */
    fun advanceFromConnectAp() {
        _uiState.value = _uiState.value.copy(step = ProvisionStep.PICK_WIFI)
        scanNetworks()
    }

    fun updateSelectedSsid(ssid: String) {
        _uiState.value = _uiState.value.copy(selectedSsid = ssid, error = null)
    }

    fun updatePassword(pw: String) {
        _uiState.value = _uiState.value.copy(password = pw)
    }

    fun updateMdns(host: String) {
        _uiState.value = _uiState.value.copy(mdnsHostname = host)
    }

    /** Open Android system WiFi settings so the user can switch networks. */
    fun openWifiSettings() {
        context.startActivity(
            Intent(Settings.ACTION_WIFI_SETTINGS).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK
            }
        )
    }

    // ── Step 2: scan networks ──────────────────────────────────────────

    fun scanNetworks() {
        _uiState.value = _uiState.value.copy(isScanning = true, networks = emptyList(), error = null)
        viewModelScope.launch {
            val networks = withContext(Dispatchers.IO) {
                try {
                    val req = Request.Builder()
                        .url("$AP_BASE/api/wifi_scan")
                        .get()
                        .build()
                    val resp = apClient.newCall(req).execute()
                    if (resp.isSuccessful) {
                        val body = resp.body?.string() ?: return@withContext emptyList()
                        parseNetworks(body)
                    } else {
                        emptyList()
                    }
                } catch (_: Exception) {
                    emptyList<WifiNetworkInfo>()
                }
            }
            _uiState.value = _uiState.value.copy(
                isScanning = false,
                networks = networks,
                error = if (networks.isEmpty()) "Nessuna rete trovata. Sei connesso ad AquariumSetup?" else null
            )
        }
    }

    private fun parseNetworks(json: String): List<WifiNetworkInfo> {
        return try {
            val adapter = moshi.adapter(WifiScanResponse::class.java)
            adapter.fromJson(json)?.networks ?: emptyList()
        } catch (_: Exception) {
            emptyList()
        }
    }

    // ── Step 3: send credentials to ESP ───────────────────────────────

    fun applyCredentials() {
        val state = _uiState.value
        if (state.selectedSsid.isBlank()) {
            _uiState.value = state.copy(error = "Seleziona una rete WiFi")
            return
        }
        _uiState.value = state.copy(step = ProvisionStep.APPLYING, isLoading = true, error = null)

        viewModelScope.launch {
            // Build JSON body manually to avoid adding a Moshi adapter for a one-off type
            val ssidEsc = state.selectedSsid.replace("\\", "\\\\").replace("\"", "\\\"")
            val pwEsc = state.password.replace("\\", "\\\\").replace("\"", "\\\"")
            val mdnsEsc = state.mdnsHostname.replace("\\", "\\\\").replace("\"", "\\\"")
            val bodyJson = """{"ssid":"$ssidEsc","password":"$pwEsc","mdns":"$mdnsEsc"}"""

            val success = withContext(Dispatchers.IO) {
                try {
                    val req = Request.Builder()
                        .url("$AP_BASE/api/provision")
                        .post(bodyJson.toRequestBody("application/json".toMediaType()))
                        .build()
                    val resp = apClient.newCall(req).execute()
                    resp.isSuccessful
                } catch (_: Exception) {
                    // A network drop immediately after the response is normal
                    // (ESP switches WiFi mode which kills the AP link)
                    true
                }
            }

            if (success) {
                // Pre-save connection settings so step 4 can verify them
                val settings = ConnectionSettings(
                    host = "${state.mdnsHostname}.local",
                    port = 443,
                    useHttps = true,
                    username = "admin"
                )
                repository.saveConnectionSettings(settings)
                _uiState.value = _uiState.value.copy(step = ProvisionStep.RECONNECT, isLoading = false)
            } else {
                _uiState.value = _uiState.value.copy(
                    step = ProvisionStep.PICK_WIFI,
                    isLoading = false,
                    error = "Errore durante la configurazione. Riprova."
                )
            }
        }
    }

    // ── Step 4: verify ESP is reachable on the home network ───────────

    fun verifyConnection() {
        _uiState.value = _uiState.value.copy(isLoading = true, error = null)
        val host = _uiState.value.mdnsHostname
        viewModelScope.launch {
            val reachable = withContext(Dispatchers.IO) {
                // Try plain HTTP first (port 80, no auth required for /api/health on captive portal)
                // then HTTPS as fallback once the main web_server is up.
                listOf(
                    "http://$host.local/api/health",
                    "http://$host.local/",
                ).any { url ->
                    try {
                        val checkClient = OkHttpClient.Builder()
                            .connectTimeout(5, TimeUnit.SECONDS)
                            .readTimeout(5, TimeUnit.SECONDS)
                            .hostnameVerifier { _, _ -> true }
                            .build()
                        val resp = checkClient.newCall(
                            Request.Builder().url(url).get().build()
                        ).execute()
                        /* Any valid HTTP response (including 4xx/5xx) means the
                         * ESP is up and listening; we just need reachability. */
                        resp.code in 100..599
                    } catch (_: Exception) {
                        false
                    }
                }
            }
            if (reachable) {
                _uiState.value = _uiState.value.copy(step = ProvisionStep.SERVICES, isLoading = false)
            } else {
                _uiState.value = _uiState.value.copy(
                    isLoading = false,
                    error = "Controller non raggiungibile. Assicurati di essere sulla stessa rete WiFi di casa."
                )
            }
        }
    }

    // ── Step 5: quick-setup optional services ─────────────────────────

    fun updateTelegramToken(t: String) { _uiState.value = _uiState.value.copy(telegramToken = t) }
    fun updateTelegramChatId(c: String) { _uiState.value = _uiState.value.copy(telegramChatId = c) }
    fun updateDuckDnsDomain(d: String) { _uiState.value = _uiState.value.copy(duckDnsDomain = d) }
    fun updateDuckDnsToken(t: String) { _uiState.value = _uiState.value.copy(duckDnsToken = t) }

    /**
     * Login with the default credentials, apply Telegram and/or DuckDNS
     * settings if provided, then navigate to the Login screen.
     *
     * If login fails (user may have changed creds during a previous setup)
     * we still navigate forward – the user will re-login manually.
     */
    fun saveServicesAndFinish() {
        val state = _uiState.value
        _uiState.value = state.copy(servicesSaving = true)
        viewModelScope.launch {
            // Login with factory-default credentials (admin / aquarium).
            // If the user changed credentials during a previous setup attempt
            // this will fail and we navigate forward anyway so they can re-login.
            val loginResult = repository.login("admin", "aquarium")
            if (loginResult.isSuccess) {
                if (state.telegramToken.isNotBlank() || state.telegramChatId.isNotBlank()) {
                    repository.postTelegram(
                        TelegramRequest(
                            botToken = state.telegramToken.ifBlank { null },
                            chatId    = state.telegramChatId.ifBlank { null },
                            enabled   = state.telegramToken.isNotBlank()
                        )
                    )
                }
                if (state.duckDnsDomain.isNotBlank() || state.duckDnsToken.isNotBlank()) {
                    repository.postDuckDns(
                        DuckDnsRequest(
                            domain  = state.duckDnsDomain.ifBlank { null },
                            token   = state.duckDnsToken.ifBlank { null },
                            enabled = state.duckDnsDomain.isNotBlank()
                        )
                    )
                }
            }
            _uiState.value = _uiState.value.copy(servicesSaving = false, navigateToLogin = true)
        }
    }

    fun skipServices() {
        _uiState.value = _uiState.value.copy(navigateToLogin = true)
    }

    fun clearError() { _uiState.value = _uiState.value.copy(error = null) }
    fun clearNavigation() { _uiState.value = _uiState.value.copy(navigateToLogin = false) }
}
