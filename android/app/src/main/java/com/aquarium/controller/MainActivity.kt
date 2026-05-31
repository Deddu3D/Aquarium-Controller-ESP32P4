package com.aquarium.controller

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.runtime.*
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.aquarium.controller.data.PrefsRepository
import com.aquarium.controller.ui.*
import com.aquarium.controller.ui.theme.AquariumTheme
import com.aquarium.controller.ui.wizard.SetupWizardScreen

private const val ROUTE_WELCOME = "welcome"
private const val ROUTE_CONNECT = "connect"
private const val ROUTE_WIZARD = "wizard"
private const val ROUTE_WEBVIEW = "webview"

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val prefs = PrefsRepository(this)

        setContent {
            AquariumTheme {
                val navController = rememberNavController()

                // Determine the start destination:
                // If the user has already completed setup, go straight to WebView.
                val startDestination = if (prefs.setupComplete && prefs.espUrl != null)
                    ROUTE_WEBVIEW else ROUTE_WELCOME

                NavHost(navController = navController, startDestination = startDestination) {

                    composable(ROUTE_WELCOME) {
                        WelcomeScreen(
                            onFirstSetup = { navController.navigate(ROUTE_WIZARD) },
                            onAlreadyConfigured = { navController.navigate(ROUTE_CONNECT) },
                        )
                    }

                    composable(ROUTE_CONNECT) {
                        ConnectScreen(
                            onConnect = { url ->
                                prefs.espUrl = url
                                prefs.setupComplete = true
                                navController.navigate(ROUTE_WEBVIEW) {
                                    popUpTo(ROUTE_WELCOME) { inclusive = true }
                                }
                            },
                            onBack = { navController.popBackStack() },
                        )
                    }

                    composable(ROUTE_WIZARD) {
                        SetupWizardScreen(
                            onSetupComplete = { url ->
                                // PrefsRepository already saved by the ViewModel.
                                navController.navigate(ROUTE_WEBVIEW) {
                                    popUpTo(ROUTE_WELCOME) { inclusive = true }
                                }
                            },
                        )
                    }

                    composable(ROUTE_WEBVIEW) {
                        val url = prefs.espUrl ?: ""
                        WebViewScreen(
                            url = url,
                            onChangeUrl = {
                                prefs.setupComplete = false
                                prefs.espUrl = null
                                navController.navigate(ROUTE_WELCOME) {
                                    popUpTo(ROUTE_WEBVIEW) { inclusive = true }
                                }
                            },
                        )
                    }
                }
            }
        }
    }
}
