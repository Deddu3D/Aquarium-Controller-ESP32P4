package com.aquarium.controller

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.aquarium.controller.repository.AquariumRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.launch
import javax.inject.Inject

/**
 * Application-scoped ViewModel attached to MainActivity.
 *
 * Restores persisted connection settings (host, port, HTTPS) as soon as
 * the app process starts so that the correct base URL is used for all HTTP calls.
 */
@HiltViewModel
class MainViewModel @Inject constructor(
    private val repository: AquariumRepository
) : ViewModel() {

    init {
        viewModelScope.launch {
            repository.initFromPrefs()
        }
    }
}
