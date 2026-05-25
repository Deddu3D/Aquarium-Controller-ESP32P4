package com.aquarium.controller.ui.login

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.data.auth.GoogleAuthManager
import com.aquarium.controller.data.prefs.ConnectionPreferences
import com.google.android.gms.auth.api.signin.GoogleSignInAccount
import com.google.android.gms.common.api.ApiException
import com.google.android.gms.common.api.CommonStatusCodes
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import javax.inject.Inject

data class GoogleSignInUiState(
    val isLoading: Boolean = false,
    val error: String? = null,
    /** Navigate to Home (has at least one device) */
    val navigateToHome: Boolean = false,
    /** Navigate to AddDevice (no device configured yet) */
    val navigateToAddDevice: Boolean = false
)

@HiltViewModel
class GoogleSignInViewModel @Inject constructor(
    private val authManager: GoogleAuthManager,
    private val connectionPrefs: ConnectionPreferences
) : ViewModel() {

    private val _uiState = MutableStateFlow(GoogleSignInUiState())
    val uiState: StateFlow<GoogleSignInUiState> = _uiState.asStateFlow()

    init {
        // Check if already signed in with a device configured
        viewModelScope.launch {
            val account = authManager.getLastSignedInAccount()
            if (account != null) {
                handleSignedInAccount(account)
            }
        }
    }

    fun onSignInResult(result: Result<GoogleSignInAccount>) {
        result.fold(
            onSuccess = { account ->
                viewModelScope.launch { handleSignedInAccount(account) }
            },
            onFailure = { e ->
                val message = when ((e as? ApiException)?.statusCode) {
                    CommonStatusCodes.DEVELOPER_ERROR ->
                        "Sign-in configuration error. Check OAuth client setup."
                    CommonStatusCodes.NETWORK_ERROR ->
                        "Network error. Check your internet connection and try again."
                    CommonStatusCodes.TIMEOUT ->
                        "Sign-in timed out. Try again."
                    else ->
                        "Sign-in failed (code ${(e as? ApiException)?.statusCode ?: "unknown"})."
                }
                _uiState.value = _uiState.value.copy(isLoading = false, error = message)
            }
        )
    }

    /** Called when the user explicitly cancels the sign-in dialog. */
    fun onSignInCancelled() {
        _uiState.value = _uiState.value.copy(isLoading = false)
    }

    private suspend fun handleSignedInAccount(account: GoogleSignInAccount) {
        _uiState.value = _uiState.value.copy(isLoading = true)
        connectionPrefs.saveGoogleUser(
            userId = account.id ?: "",
            email = account.email ?: "",
            displayName = account.displayName ?: ""
        )
        val devices = connectionPrefs.devices.first()
        _uiState.value = _uiState.value.copy(
            isLoading = false,
            navigateToHome = devices.isNotEmpty(),
            navigateToAddDevice = devices.isEmpty()
        )
    }

    fun setLoading(loading: Boolean) {
        _uiState.value = _uiState.value.copy(isLoading = loading)
    }

    fun clearError() { _uiState.value = _uiState.value.copy(error = null) }
    fun clearNavigation() {
        _uiState.value = _uiState.value.copy(
            navigateToHome = false,
            navigateToAddDevice = false
        )
    }
}
