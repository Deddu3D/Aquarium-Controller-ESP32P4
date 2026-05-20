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

    // Refreshes data silently without replacing the current Success state with Loading.
    private fun silentRefresh() {
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
            }
            // On failure during a silent refresh keep the current (optimistic) state visible.
        }
    }

    // ── Commands ─────────────────────────────────────────────────────────

    fun toggleRelay(index: Int, currentOn: Boolean) {
        val previous = _uiState.value
        // Optimistic update: flip the relay state immediately so the Switch doesn't snap back.
        if (previous is HomeUiState.Success) {
            val updatedRelays = previous.relays.relays.map { relay ->
                if (relay.index == index) relay.copy(on = !currentOn) else relay
            }
            _uiState.value = previous.copy(relays = previous.relays.copy(relays = updatedRelays))
        }
        viewModelScope.launch {
            repository.toggleRelay(index, !currentOn).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed to toggle relay: ${it.message}"
                }
            )
        }
    }

    fun setLedOn(on: Boolean) {
        val previous = _uiState.value
        // Optimistic update: flip the LED on/off state immediately.
        if (previous is HomeUiState.Success) {
            _uiState.value = previous.copy(leds = previous.leds.copy(on = on))
        }
        viewModelScope.launch {
            repository.postLeds(LedRequest(on = on)).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed to toggle lights: ${it.message}"
                }
            )
        }
    }

    fun startFeeding() {
        val previous = _uiState.value
        if (previous is HomeUiState.Success) {
            _uiState.value = previous.copy(feeding = previous.feeding.copy(active = true))
        }
        viewModelScope.launch {
            repository.postFeeding(FeedingRequest(action = "start")).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed to start feeding: ${it.message}"
                }
            )
        }
    }

    fun stopFeeding() {
        val previous = _uiState.value
        if (previous is HomeUiState.Success) {
            _uiState.value = previous.copy(feeding = previous.feeding.copy(active = false, remainingS = 0))
        }
        viewModelScope.launch {
            repository.postFeeding(FeedingRequest(action = "stop")).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed to stop feeding: ${it.message}"
                }
            )
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }
}
