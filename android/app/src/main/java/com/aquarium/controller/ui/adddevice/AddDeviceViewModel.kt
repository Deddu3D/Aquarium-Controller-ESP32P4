package com.aquarium.controller.ui.adddevice

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.model.EspDevice
import com.aquarium.controller.data.prefs.ConnectionPreferences
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

data class AddDeviceUiState(
    val deviceName: String = "My Aquarium",
    val duckDnsDomain: String = "",
    val isTesting: Boolean = false,
    val isSaving: Boolean = false,
    val testResult: String? = null,
    val error: String? = null,
    val navigateToHome: Boolean = false
)

@HiltViewModel
class AddDeviceViewModel @Inject constructor(
    private val connectionPrefs: ConnectionPreferences,
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow(AddDeviceUiState())
    val uiState: StateFlow<AddDeviceUiState> = _uiState.asStateFlow()

    fun updateDeviceName(name: String) { _uiState.value = _uiState.value.copy(deviceName = name) }

    fun updateDomain(domain: String) {
        // Accept both "my-aquarium" and "my-aquarium.duckdns.org"
        val normalized = domain
            .trim()
            .removeSuffix(".duckdns.org")
            .removeSuffix(".duckdns.org/")
        _uiState.value = _uiState.value.copy(duckDnsDomain = normalized, testResult = null)
    }

    fun testConnection() {
        val domain = _uiState.value.duckDnsDomain
        if (domain.isBlank()) {
            _uiState.value = _uiState.value.copy(error = "Please enter a DuckDNS domain")
            return
        }
        _uiState.value = _uiState.value.copy(isTesting = true, testResult = null)
        viewModelScope.launch {
            repository.setBaseUrl("https://$domain.duckdns.org/")
            val result = repository.getStatus()
            _uiState.value = _uiState.value.copy(
                isTesting = false,
                testResult = if (result.isSuccess) "✓ Connected successfully" else "✗ ${result.exceptionOrNull()?.message}"
            )
        }
    }

    fun saveDevice() {
        val state = _uiState.value
        if (state.duckDnsDomain.isBlank()) {
            _uiState.value = state.copy(error = "Please enter a DuckDNS domain")
            return
        }
        _uiState.value = state.copy(isSaving = true)
        viewModelScope.launch {
            val device = EspDevice(
                name = state.deviceName.ifBlank { "My Aquarium" },
                duckDnsDomain = state.duckDnsDomain
            )
            connectionPrefs.addDevice(device)
            connectionPrefs.selectDevice(device.id)
            repository.setBaseUrl(device.baseUrl)
            _uiState.value = _uiState.value.copy(isSaving = false, navigateToHome = true)
        }
    }

    /** Pre-fill domain when arriving from the provisioning wizard. */
    fun preFillDomain(domain: String) {
        if (domain.isNotBlank()) {
            _uiState.value = _uiState.value.copy(duckDnsDomain = domain.removeSuffix(".duckdns.org"))
        }
    }

    fun clearError() { _uiState.value = _uiState.value.copy(error = null) }
    fun clearNavigation() { _uiState.value = _uiState.value.copy(navigateToHome = false) }
}
