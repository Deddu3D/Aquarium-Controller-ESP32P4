package com.aquarium.controller

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.aquarium.controller.ui.nav.AppNavigation
import com.aquarium.controller.ui.theme.AquariumTheme
import dagger.hilt.android.AndroidEntryPoint

@AndroidEntryPoint
class MainActivity : ComponentActivity() {

    // Instantiating MainViewModel here ensures initFromPrefs() runs as soon
    // as the Activity is created, before any screen ViewModel is active.
    @Suppress("unused")
    private val mainViewModel: MainViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            AquariumTheme {
                AppNavigation()
            }
        }
    }
}
