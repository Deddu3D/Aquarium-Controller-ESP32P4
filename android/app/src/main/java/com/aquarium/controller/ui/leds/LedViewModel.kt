package com.aquarium.controller.ui.leds

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

data class LedUiData(
    val leds: LedResponse,
    val schedule: LedScheduleResponse,
    val presets: LedPresetsResponse,
    val scene: SceneResponse,
    val dailyCycle: DailyCycleResponse
)

sealed class LedUiState {
    object Loading : LedUiState()
    data class Success(val data: LedUiData) : LedUiState()
    data class Error(val message: String) : LedUiState()
}

@HiltViewModel
class LedViewModel @Inject constructor(
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow<LedUiState>(LedUiState.Loading)
    val uiState: StateFlow<LedUiState> = _uiState.asStateFlow()

    private val _snackbarMessage = MutableStateFlow<String?>(null)
    val snackbarMessage: StateFlow<String?> = _snackbarMessage.asStateFlow()

    init { load() }

    fun load() {
        _uiState.value = LedUiState.Loading
        viewModelScope.launch {
            val ledsResult = repository.getLeds()
            val scheduleResult = repository.getLedSchedule()
            val presetsResult = repository.getLedPresets()
            val sceneResult = repository.getScene()
            val dailyCycleResult = repository.getDailyCycle()

            if (ledsResult.isSuccess && scheduleResult.isSuccess &&
                presetsResult.isSuccess && sceneResult.isSuccess && dailyCycleResult.isSuccess) {
                _uiState.value = LedUiState.Success(
                    LedUiData(
                        leds = ledsResult.getOrThrow(),
                        schedule = scheduleResult.getOrThrow(),
                        presets = presetsResult.getOrThrow(),
                        scene = sceneResult.getOrThrow(),
                        dailyCycle = dailyCycleResult.getOrThrow()
                    )
                )
            } else {
                val err = ledsResult.exceptionOrNull() ?: scheduleResult.exceptionOrNull()
                    ?: presetsResult.exceptionOrNull() ?: sceneResult.exceptionOrNull()
                    ?: dailyCycleResult.exceptionOrNull()
                _uiState.value = LedUiState.Error(err?.message ?: "Unknown error")
            }
        }
    }

    // Refreshes data silently without replacing the current Success state with Loading.
    private fun silentRefresh() {
        viewModelScope.launch {
            val ledsResult = repository.getLeds()
            val scheduleResult = repository.getLedSchedule()
            val presetsResult = repository.getLedPresets()
            val sceneResult = repository.getScene()
            val dailyCycleResult = repository.getDailyCycle()

            if (ledsResult.isSuccess && scheduleResult.isSuccess &&
                presetsResult.isSuccess && sceneResult.isSuccess && dailyCycleResult.isSuccess) {
                _uiState.value = LedUiState.Success(
                    LedUiData(
                        leds = ledsResult.getOrThrow(),
                        schedule = scheduleResult.getOrThrow(),
                        presets = presetsResult.getOrThrow(),
                        scene = sceneResult.getOrThrow(),
                        dailyCycle = dailyCycleResult.getOrThrow()
                    )
                )
            }
            // On failure during a silent refresh keep the current (optimistic) state visible.
        }
    }

    fun setOn(on: Boolean) {
        val previous = _uiState.value
        // Optimistic update: flip the LED on/off state immediately so the Switch doesn't snap back.
        if (previous is LedUiState.Success) {
            _uiState.value = previous.copy(data = previous.data.copy(leds = previous.data.leds.copy(on = on)))
        }
        viewModelScope.launch {
            repository.postLeds(LedRequest(on = on)).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed: ${it.message}"
                }
            )
        }
    }

    fun setBrightness(brightness: Int) {
        viewModelScope.launch {
            repository.postLeds(LedRequest(brightness = brightness)).fold(
                onSuccess = {},
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun setColor(r: Int, g: Int, b: Int) {
        viewModelScope.launch {
            repository.postLeds(LedRequest(r = r, g = g, b = b)).fold(
                onSuccess = {},
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun startScene(sceneIndex: Int) {
        val previous = _uiState.value
        // Optimistic update: show the selected scene chip as active immediately.
        if (previous is LedUiState.Success) {
            _uiState.value = previous.copy(data = previous.data.copy(scene = previous.data.scene.copy(active = sceneIndex)))
        }
        viewModelScope.launch {
            repository.postScene(sceneIndex).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed: ${it.message}"
                }
            )
        }
    }

    fun savePreset(slot: Int, name: String) {
        viewModelScope.launch {
            repository.postLedPresets(PresetActionRequest("save", slot, name)).fold(
                onSuccess = { silentRefresh() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun loadPreset(slot: Int) {
        viewModelScope.launch {
            repository.postLedPresets(PresetActionRequest("load", slot)).fold(
                onSuccess = { silentRefresh() },
                onFailure = { _snackbarMessage.value = "Failed: ${it.message}" }
            )
        }
    }

    fun updateSchedule(request: LedScheduleRequest) {
        val previous = _uiState.value
        // Optimistic update for the enabled flag so the Schedule Switch doesn't snap back.
        if (previous is LedUiState.Success && request.enabled != null) {
            _uiState.value = previous.copy(
                data = previous.data.copy(schedule = previous.data.schedule.copy(enabled = request.enabled))
            )
        }
        viewModelScope.launch {
            repository.postLedSchedule(request).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed: ${it.message}"
                }
            )
        }
    }

    fun setDailyCycleEnabled(enabled: Boolean) {
        val previous = _uiState.value
        // Optimistic update: flip the Daily Cycle Switch immediately.
        if (previous is LedUiState.Success) {
            _uiState.value = previous.copy(
                data = previous.data.copy(dailyCycle = previous.data.dailyCycle.copy(enabled = enabled))
            )
        }
        viewModelScope.launch {
            repository.postDailyCycle(DailyCycleRequest(enabled = enabled)).fold(
                onSuccess = { silentRefresh() },
                onFailure = {
                    _uiState.value = previous  // revert optimistic update
                    _snackbarMessage.value = "Failed: ${it.message}"
                }
            )
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }
}
