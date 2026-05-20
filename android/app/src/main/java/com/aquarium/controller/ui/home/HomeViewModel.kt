package com.aquarium.controller.ui.home

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.model.FeedingResponse
import com.aquarium.controller.data.model.FeedingRequest
import com.aquarium.controller.data.model.HealthResponse
import com.aquarium.controller.data.model.LedRequest
import com.aquarium.controller.data.model.LedResponse
import com.aquarium.controller.data.model.RelaysResponse
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
        val leds: LedResponse
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
    }

    fun load() {
        _uiState.value = HomeUiState.Loading
        viewModelScope.launch {
            val statusResult  = repository.getStatus()
            val healthResult  = repository.getHealth()
            val relaysResult  = repository.getRelays()
            val feedingResult = repository.getFeeding()
            val ledsResult    = repository.getLeds()

            if (statusResult.isSuccess && healthResult.isSuccess &&
                relaysResult.isSuccess && feedingResult.isSuccess && ledsResult.isSuccess) {
                _uiState.value = HomeUiState.Success(
                    status  = statusResult.getOrThrow(),
                    health  = healthResult.getOrThrow(),
                    relays  = relaysResult.getOrThrow(),
                    feeding = feedingResult.getOrThrow(),
                    leds    = ledsResult.getOrThrow()
                )
            } else {
                val err = statusResult.exceptionOrNull()
                    ?: healthResult.exceptionOrNull()
                    ?: relaysResult.exceptionOrNull()
                    ?: feedingResult.exceptionOrNull()
                    ?: ledsResult.exceptionOrNull()
                _uiState.value = HomeUiState.Error(err?.message ?: "Unknown error")
            }
        }
    }

    // ── Commands ─────────────────────────────────────────────────────────

    fun toggleRelay(index: Int, currentOn: Boolean) {
        viewModelScope.launch {
            repository.toggleRelay(index, !currentOn).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed to toggle relay: ${it.message}" }
            )
        }
    }

    fun setLedOn(on: Boolean) {
        viewModelScope.launch {
            repository.postLeds(LedRequest(on = on)).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed to toggle lights: ${it.message}" }
            )
        }
    }

    fun startFeeding() {
        viewModelScope.launch {
            repository.postFeeding(FeedingRequest(action = "start")).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed to start feeding: ${it.message}" }
            )
        }
    }

    fun stopFeeding() {
        viewModelScope.launch {
            repository.postFeeding(FeedingRequest(action = "stop")).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed to stop feeding: ${it.message}" }
            )
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }
}
