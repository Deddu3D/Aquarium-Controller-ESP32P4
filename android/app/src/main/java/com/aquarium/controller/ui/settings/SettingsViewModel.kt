package com.aquarium.controller.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.model.*
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

data class SettingsUiData(
    val telegram: TelegramResponse,
    val duckDns: DuckDnsResponse,
    val timezone: TimezoneResponse,
    val mdns: MdnsResponse,
    val otaStatus: OtaStatusResponse
)

sealed class SettingsUiState {
    object Loading : SettingsUiState()
    data class Success(val data: SettingsUiData) : SettingsUiState()
    data class Error(val message: String) : SettingsUiState()
}

@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow<SettingsUiState>(SettingsUiState.Loading)
    val uiState: StateFlow<SettingsUiState> = _uiState.asStateFlow()

    private val _snackbarMessage = MutableStateFlow<String?>(null)
    val snackbarMessage: StateFlow<String?> = _snackbarMessage.asStateFlow()

    private val _navigateToConnect = MutableStateFlow(false)
    val navigateToConnect: StateFlow<Boolean> = _navigateToConnect.asStateFlow()

    init { load() }

    fun load() {
        _uiState.value = SettingsUiState.Loading
        viewModelScope.launch {
            val telegramResult = repository.getTelegram()
            val duckDnsResult = repository.getDuckDns()
            val timezoneResult = repository.getTimezone()
            val mdnsResult = repository.getMdns()
            val otaResult = repository.getOtaStatus()

            if (telegramResult.isSuccess && duckDnsResult.isSuccess &&
                timezoneResult.isSuccess && mdnsResult.isSuccess && otaResult.isSuccess) {
                _uiState.value = SettingsUiState.Success(
                    SettingsUiData(
                        telegram = telegramResult.getOrThrow(),
                        duckDns = duckDnsResult.getOrThrow(),
                        timezone = timezoneResult.getOrThrow(),
                        mdns = mdnsResult.getOrThrow(),
                        otaStatus = otaResult.getOrThrow()
                    )
                )
            } else {
                val err = telegramResult.exceptionOrNull() ?: duckDnsResult.exceptionOrNull()
                    ?: timezoneResult.exceptionOrNull() ?: mdnsResult.exceptionOrNull()
                    ?: otaResult.exceptionOrNull()
                _uiState.value = SettingsUiState.Error(err?.message ?: "Unknown error")
            }
        }
    }

    fun updateTelegram(request: TelegramRequest) {
        viewModelScope.launch {
            repository.postTelegram(request).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun telegramTest() {
        viewModelScope.launch {
            repository.telegramTest().fold(
                onSuccess = { _snackbarMessage.value = "Test message sent" },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateDuckDns(request: DuckDnsRequest) {
        viewModelScope.launch {
            repository.postDuckDns(request).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun duckDnsUpdate() {
        viewModelScope.launch {
            repository.duckDnsUpdate().fold(
                onSuccess = { _snackbarMessage.value = "DuckDNS updated" },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateTimezone(tz: String) {
        viewModelScope.launch {
            repository.postTimezone(tz).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateMdns(request: MdnsRequest) {
        viewModelScope.launch {
            repository.postMdns(request).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun startOta(url: String) {
        viewModelScope.launch {
            repository.postOta(url).fold(
                onSuccess = { _snackbarMessage.value = "OTA started" },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun changeAuth(username: String, password: String) {
        viewModelScope.launch {
            repository.changeAuth(username, password).fold(
                onSuccess = { _snackbarMessage.value = "Credentials updated" },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun exportConfig() {
        viewModelScope.launch {
            repository.exportConfig().fold(
                onSuccess = { _snackbarMessage.value = "Config exported" },
                onFailure = { _snackbarMessage.value = "Export failed: ${it.message}" }
            )
        }
    }

    fun factoryReset() {
        viewModelScope.launch {
            repository.factoryReset().fold(
                onSuccess = { _snackbarMessage.value = "Factory reset initiated" },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun logout() {
        viewModelScope.launch {
            repository.logout()
            _navigateToConnect.value = true
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }
    fun clearNavigation() { _navigateToConnect.value = false }
}
