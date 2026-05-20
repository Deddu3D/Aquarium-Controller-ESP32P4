package com.aquarium.controller.ui.temperature

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.model.HeaterRequest
import com.aquarium.controller.data.model.HeaterResponse
import com.aquarium.controller.data.model.TemperatureHistoryResponse
import com.aquarium.controller.data.model.TemperatureResponse
import com.aquarium.controller.data.model.WsStatus
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

data class TempUiData(
    val current: TemperatureResponse,
    val history: TemperatureHistoryResponse,
    val heater: HeaterResponse
)

sealed class TempUiState {
    object Loading : TempUiState()
    data class Success(val data: TempUiData) : TempUiState()
    data class Error(val message: String) : TempUiState()
}

@HiltViewModel
class TempViewModel @Inject constructor(
    private val repository: AquariumRepository
) : ViewModel() {

    private val _uiState = MutableStateFlow<TempUiState>(TempUiState.Loading)
    val uiState: StateFlow<TempUiState> = _uiState.asStateFlow()

    val wsStatus: StateFlow<WsStatus> = repository.webSocketManager.status

    private val _snackbarMessage = MutableStateFlow<String?>(null)
    val snackbarMessage: StateFlow<String?> = _snackbarMessage.asStateFlow()

    private val _exportData = MutableStateFlow<String?>(null)
    val exportData: StateFlow<String?> = _exportData.asStateFlow()

    init { load() }

    fun load() {
        _uiState.value = TempUiState.Loading
        viewModelScope.launch {
            val currentResult = repository.getTemperature()
            val historyResult = repository.getTemperatureHistory()
            val heaterResult = repository.getHeater()

            if (currentResult.isSuccess && historyResult.isSuccess && heaterResult.isSuccess) {
                _uiState.value = TempUiState.Success(
                    TempUiData(
                        current = currentResult.getOrThrow(),
                        history = historyResult.getOrThrow(),
                        heater = heaterResult.getOrThrow()
                    )
                )
            } else {
                val err = currentResult.exceptionOrNull()
                    ?: historyResult.exceptionOrNull()
                    ?: heaterResult.exceptionOrNull()
                _uiState.value = TempUiState.Error(err?.message ?: "Unknown error")
            }
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

    fun exportCsv() {
        viewModelScope.launch {
            repository.exportTemperatureCsv().fold(
                onSuccess = { body ->
                    body.use { _exportData.value = it.string() }
                    _snackbarMessage.value = "CSV ready"
                },
                onFailure = { _snackbarMessage.value = "Export failed: ${it.message}" }
            )
        }
    }

    fun clearSnackbar() { _snackbarMessage.value = null }
    fun clearExportData() { _exportData.value = null }
}
