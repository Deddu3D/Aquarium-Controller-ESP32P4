package com.aquarium.controller.ui.connect

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.prefs.ConnectionPreferences
import com.aquarium.controller.data.prefs.ConnectionSettings
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import javax.inject.Inject

data class ConnectUiState(
    val host: String = "",
    val port: String = "443",
    val useHttps: Boolean = true,
    val username: String = "admin",
    val isScanning: Boolean = false,
    val discoveredHosts: List<String> = emptyList(),
    val error: String? = null,
    val navigateToLogin: Boolean = false
)

@HiltViewModel
class ConnectViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val connectionPrefs: ConnectionPreferences,
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow(ConnectUiState())
    val uiState: StateFlow<ConnectUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            val settings = connectionPrefs.settings.first()
            _uiState.value = _uiState.value.copy(
                host = settings.host,
                port = settings.port.toString(),
                useHttps = settings.useHttps,
                username = settings.username
            )
        }
    }

    fun updateHost(host: String) { _uiState.value = _uiState.value.copy(host = host) }
    fun updatePort(port: String) { _uiState.value = _uiState.value.copy(port = port) }
    fun updateUseHttps(useHttps: Boolean) {
        val port = if (useHttps) "443" else "80"
        _uiState.value = _uiState.value.copy(useHttps = useHttps, port = port)
    }
    fun updateUsername(username: String) { _uiState.value = _uiState.value.copy(username = username) }

    fun scanMdns() {
        _uiState.value = _uiState.value.copy(isScanning = true, discoveredHosts = emptyList())
        val nsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
        val discovered = mutableListOf<String>()

        val resolveListener = object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {}
            override fun onServiceResolved(info: NsdServiceInfo) {
                val host = info.host?.hostAddress ?: info.serviceName
                discovered.add(host)
                _uiState.value = _uiState.value.copy(discoveredHosts = discovered.toList())
            }
        }

        val discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {
                _uiState.value = _uiState.value.copy(isScanning = false)
            }
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                _uiState.value = _uiState.value.copy(isScanning = false, error = "mDNS scan failed: $errorCode")
            }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                try { nsdManager.resolveService(info, resolveListener) } catch (_: Exception) {}
            }
            override fun onServiceLost(info: NsdServiceInfo) {}
        }

        viewModelScope.launch {
            try {
                nsdManager.discoverServices("_http._tcp.", NsdManager.PROTOCOL_DNS_SD, discoveryListener)
                kotlinx.coroutines.delay(5000)
                try { nsdManager.stopServiceDiscovery(discoveryListener) } catch (_: Exception) {}
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isScanning = false, error = "mDNS error: ${e.message}")
            }
        }
    }

    fun selectDiscoveredHost(host: String) {
        _uiState.value = _uiState.value.copy(host = host)
    }

    fun connect() {
        val state = _uiState.value
        if (state.host.isBlank()) {
            _uiState.value = state.copy(error = "Please enter a host")
            return
        }
        val port = state.port.toIntOrNull() ?: run {
            _uiState.value = state.copy(error = "Invalid port number")
            return
        }
        viewModelScope.launch {
            val settings = ConnectionSettings(
                host = state.host,
                port = port,
                useHttps = state.useHttps,
                username = state.username
            )
            repository.saveConnectionSettings(settings)
            _uiState.value = _uiState.value.copy(navigateToLogin = true)
        }
    }

    fun clearError() { _uiState.value = _uiState.value.copy(error = null) }
    fun clearNavigation() { _uiState.value = _uiState.value.copy(navigateToLogin = false) }
}
