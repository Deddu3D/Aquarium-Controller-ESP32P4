package com.aquarium.controller.ui.home

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.model.FeedingResponse
import com.aquarium.controller.data.model.FeedingRequest
import com.aquarium.controller.data.model.HealthResponse
import com.aquarium.controller.data.model.RelayInfo
import com.aquarium.controller.data.model.RelaysResponse
import com.aquarium.controller.data.model.RemoteStatus
import com.aquarium.controller.data.model.StatusResponse
import com.aquarium.controller.data.model.WsStatus
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

sealed class HomeUiState {
    object Loading : HomeUiState()
    data class Success(
        val status: StatusResponse,
        val health: HealthResponse,
        val relays: RelaysResponse,
        val feeding: FeedingResponse,
        /** true when data comes from MQTT (no local HTTP connection available) */
        val isRemote: Boolean = false
    ) : HomeUiState()
    data class Error(val message: String) : HomeUiState()
}

@HiltViewModel
class HomeViewModel @Inject constructor(
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow<HomeUiState>(HomeUiState.Loading)
    val uiState: StateFlow<HomeUiState> = _uiState.asStateFlow()

    val wsStatus: StateFlow<WsStatus> = repository.webSocketManager.status

    private val _snackbarMessage = MutableStateFlow<String?>(null)
    val snackbarMessage: StateFlow<String?> = _snackbarMessage.asStateFlow()

    init {
        load()

        // Collect live MQTT status updates so the UI refreshes automatically:
        //  • When already in remote mode – keep data current (ESP publishes every 30 s).
        //  • When in Error/Loading state – auto-transition to remote mode once the
        //    broker delivers the first status (handles delayed MQTT connect).
        viewModelScope.launch {
            repository.mqttRemoteManager.status.collect { mqttSt ->
                val current = _uiState.value
                when {
                    current is HomeUiState.Success && current.isRemote && mqttSt.connected ->
                        _uiState.value = buildMqttSuccessState(mqttSt)

                    (current is HomeUiState.Error || current is HomeUiState.Loading) && mqttSt.connected ->
                        _uiState.value = buildMqttSuccessState(mqttSt)
                }
            }
        }
    }

    fun load() {
        _uiState.value = HomeUiState.Loading
        viewModelScope.launch {
            val statusResult  = repository.getStatus()
            val healthResult  = repository.getHealth()
            val relaysResult  = repository.getRelays()
            val feedingResult = repository.getFeeding()

            if (statusResult.isSuccess && healthResult.isSuccess &&
                relaysResult.isSuccess && feedingResult.isSuccess) {
                _uiState.value = HomeUiState.Success(
                    status  = statusResult.getOrThrow(),
                    health  = healthResult.getOrThrow(),
                    relays  = relaysResult.getOrThrow(),
                    feeding = feedingResult.getOrThrow(),
                    isRemote = false
                )
            } else {
                // HTTP unreachable – fall back to MQTT if a status has been received
                val mqttSt = repository.mqttRemoteManager.status.value
                if (repository.mqttRemoteManager.isConnected() && mqttSt.connected) {
                    _uiState.value = buildMqttSuccessState(mqttSt)
                } else {
                    val err = statusResult.exceptionOrNull()
                        ?: healthResult.exceptionOrNull()
                        ?: relaysResult.exceptionOrNull()
                        ?: feedingResult.exceptionOrNull()
                    _uiState.value = HomeUiState.Error(err?.message ?: "Unknown error")
                }
            }
        }
    }

    // ── Commands ─────────────────────────────────────────────────────────

    fun toggleRelay(index: Int, currentOn: Boolean) {
        if (isRemoteMode()) {
            repository.mqttRemoteManager.sendRelayToggle(index, !currentOn)
            _snackbarMessage.value = "Comando inviato via MQTT"
        } else {
            viewModelScope.launch {
                repository.toggleRelay(index, !currentOn).fold(
                    onSuccess = { load() },
                    onFailure = { _snackbarMessage.value = "Failed to toggle relay: ${it.message}" }
                )
            }
        }
    }

    fun startFeeding() {
        if (isRemoteMode()) {
            repository.mqttRemoteManager.sendFeedingStart()
            _snackbarMessage.value = "Comando inviato via MQTT"
        } else {
            viewModelScope.launch {
                repository.postFeeding(FeedingRequest(action = "start")).fold(
                    onSuccess = { load() },
                    onFailure = { _snackbarMessage.value = "Failed to start feeding: ${it.message}" }
                )
            }
        }
    }

    fun stopFeeding() {
        if (isRemoteMode()) {
            repository.mqttRemoteManager.sendFeedingStop()
            _snackbarMessage.value = "Comando inviato via MQTT"
        } else {
            viewModelScope.launch {
                repository.postFeeding(FeedingRequest(action = "stop")).fold(
                    onSuccess = { load() },
                    onFailure = { _snackbarMessage.value = "Failed to stop feeding: ${it.message}" }
                )
            }
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }

    // ── Private helpers ───────────────────────────────────────────────────

    private fun isRemoteMode(): Boolean =
        (_uiState.value as? HomeUiState.Success)?.isRemote == true

    /**
     * Build a [HomeUiState.Success] from live MQTT data.
     * Fields that are unavailable via MQTT are filled with neutral placeholders.
     */
    private fun buildMqttSuccessState(mqttSt: RemoteStatus): HomeUiState.Success =
        HomeUiState.Success(
            status = StatusResponse(
                connected     = mqttSt.connected,
                ip            = "--",
                ssid          = "--",
                rssi          = 0,
                freeHeap      = mqttSt.freeHeap,
                uptimeS       = mqttSt.uptimeS,
                ntpOk         = true,
                partition     = "--",
                bootCount     = 0,
                restartReason = "--"
            ),
            health = HealthResponse(
                healthy            = true,
                wifi               = true,
                temperatureSensor  = true,
                ledStrip           = true,
                ledScheduleEnabled = false,
                tempC              = mqttSt.tempC,
                freeHeap           = mqttSt.freeHeap,
                minFreeHeap        = 0,
                uptimeS            = mqttSt.uptimeS
            ),
            relays = RelaysResponse(
                count  = 4,
                relays = listOf(
                    RelayInfo(0, mqttSt.relay0, "Relay 1", emptyList()),
                    RelayInfo(1, mqttSt.relay1, "Relay 2", emptyList()),
                    RelayInfo(2, mqttSt.relay2, "Relay 3", emptyList()),
                    RelayInfo(3, mqttSt.relay3, "Relay 4", emptyList())
                )
            ),
            feeding = FeedingResponse(
                active       = mqttSt.feedingActive,
                remainingS   = 0,
                relayIndex   = 0,
                durationMin  = 0,
                dimLights    = false,
                dimBrightness = 0
            ),
            isRemote = true
        )
}
