package com.aquarium.controller.ui.login

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.prefs.ConnectionPreferences
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import javax.inject.Inject

data class LoginUiState(
    val username: String = "",
    val password: String = "",
    val isLoading: Boolean = false,
    val error: String? = null,
    val navigateToHome: Boolean = false
)

@HiltViewModel
class LoginViewModel @Inject constructor(
    private val repository: AquariumRepository,
    private val connectionPrefs: ConnectionPreferences
) : ViewModel() {

    private val _uiState = MutableStateFlow(LoginUiState())
    val uiState: StateFlow<LoginUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            val settings = connectionPrefs.settings.first()
            _uiState.value = _uiState.value.copy(username = settings.username)
            repository.initFromPrefs()
        }
    }

    fun updateUsername(v: String) { _uiState.value = _uiState.value.copy(username = v) }
    fun updatePassword(v: String) { _uiState.value = _uiState.value.copy(password = v) }

    fun login() {
        val state = _uiState.value
        if (state.username.isBlank() || state.password.isBlank()) {
            _uiState.value = state.copy(error = "Username and password are required")
            return
        }
        _uiState.value = state.copy(isLoading = true, error = null)
        viewModelScope.launch {
            val settings = connectionPrefs.settings.first()
            repository.login(state.username, state.password).fold(
                onSuccess = {
                    if (it.ok) {
                        connectionPrefs.saveUsername(state.username)
                        repository.connectWebSocket(settings.baseUrl)
                        _uiState.value = _uiState.value.copy(isLoading = false, navigateToHome = true)
                    } else {
                        _uiState.value = _uiState.value.copy(isLoading = false, error = "Login failed")
                    }
                },
                onFailure = {
                    _uiState.value = _uiState.value.copy(isLoading = false, error = it.message ?: "Login failed")
                }
            )
        }
    }

    fun clearError() { _uiState.value = _uiState.value.copy(error = null) }
    fun clearNavigation() { _uiState.value = _uiState.value.copy(navigateToHome = false) }
}
