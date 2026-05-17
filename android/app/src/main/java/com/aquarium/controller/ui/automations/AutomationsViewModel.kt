package com.aquarium.controller.ui.automations

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

data class AutomationsUiData(
    val relays: RelaysResponse,
    val co2: Co2Response,
    val heater: HeaterResponse,
    val feeding: FeedingResponse
)

sealed class AutomationsUiState {
    object Loading : AutomationsUiState()
    data class Success(val data: AutomationsUiData) : AutomationsUiState()
    data class Error(val message: String) : AutomationsUiState()
}

@HiltViewModel
class AutomationsViewModel @Inject constructor(
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow<AutomationsUiState>(AutomationsUiState.Loading)
    val uiState: StateFlow<AutomationsUiState> = _uiState.asStateFlow()

    private val _snackbarMessage = MutableStateFlow<String?>(null)
    val snackbarMessage: StateFlow<String?> = _snackbarMessage.asStateFlow()

    init { load() }

    fun load() {
        _uiState.value = AutomationsUiState.Loading
        viewModelScope.launch {
            val relaysResult = repository.getRelays()
            val co2Result = repository.getCo2()
            val heaterResult = repository.getHeater()
            val feedingResult = repository.getFeeding()

            if (relaysResult.isSuccess && co2Result.isSuccess &&
                heaterResult.isSuccess && feedingResult.isSuccess) {
                _uiState.value = AutomationsUiState.Success(
                    AutomationsUiData(
                        relays = relaysResult.getOrThrow(),
                        co2 = co2Result.getOrThrow(),
                        heater = heaterResult.getOrThrow(),
                        feeding = feedingResult.getOrThrow()
                    )
                )
            } else {
                val err = relaysResult.exceptionOrNull() ?: co2Result.exceptionOrNull()
                    ?: heaterResult.exceptionOrNull() ?: feedingResult.exceptionOrNull()
                _uiState.value = AutomationsUiState.Error(err?.message ?: "Unknown error")
            }
        }
    }

    fun toggleRelay(index: Int, on: Boolean) {
        viewModelScope.launch {
            repository.toggleRelay(index, on).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateCo2(request: Co2Request) {
        viewModelScope.launch {
            repository.postCo2(request).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateHeater(request: HeaterRequest) {
        viewModelScope.launch {
            repository.postHeater(request).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun startFeeding() {
        viewModelScope.launch {
            repository.postFeeding(FeedingRequest(action = "start")).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun stopFeeding() {
        viewModelScope.launch {
            repository.postFeeding(FeedingRequest(action = "stop")).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateFeedingConfig(request: FeedingRequest) {
        viewModelScope.launch {
            repository.postFeeding(request).fold(
                onSuccess = { load() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }
}
