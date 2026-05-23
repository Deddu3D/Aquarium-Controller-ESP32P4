package com.aquarium.controller

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.aquarium.controller.data.auth.GoogleAuthManager
import com.aquarium.controller.ui.nav.AppNavigation
import com.aquarium.controller.ui.theme.AquariumTheme
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject

@AndroidEntryPoint
class MainActivity : ComponentActivity() {

    @Suppress("unused")
    private val mainViewModel: MainViewModel by viewModels()

    @Inject
    lateinit var authManager: GoogleAuthManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            AquariumTheme {
                AppNavigation(authManager = authManager)
            }
        }
    }
}
