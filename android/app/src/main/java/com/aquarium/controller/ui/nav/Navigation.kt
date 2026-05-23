package com.aquarium.controller.ui.nav

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.aquarium.controller.data.auth.GoogleAuthManager
import com.aquarium.controller.ui.adddevice.AddDeviceScreen
import com.aquarium.controller.ui.automations.AutomationsScreen
import com.aquarium.controller.ui.home.HomeScreen
import com.aquarium.controller.ui.leds.LedScreen
import com.aquarium.controller.ui.login.GoogleSignInScreen
import com.aquarium.controller.ui.provision.ProvisionScreen
import com.aquarium.controller.ui.settings.SettingsScreen
import com.aquarium.controller.ui.temperature.TempScreen
import dagger.hilt.android.EntryPointAccessors
import androidx.compose.ui.platform.LocalContext

sealed class Screen(val route: String) {
    object GoogleSignIn : Screen("google_signin")
    object AddDevice : Screen("add_device?domain={domain}") {
        fun createRoute(domain: String = "") = "add_device?domain=$domain"
        const val route = "add_device?domain={domain}"
    }
    object Provision : Screen("provision")
    object Home : Screen("home/{tab}") {
        fun createRoute(tab: Int = 0) = "home/$tab"
    }
    object Leds : Screen("leds")
    object Temperature : Screen("temperature")
    object Automations : Screen("automations")
    object Settings : Screen("settings")
}

@Composable
fun AppNavigation(authManager: GoogleAuthManager) {
    val navController = rememberNavController()
    NavHost(navController = navController, startDestination = Screen.GoogleSignIn.route) {
        composable(Screen.GoogleSignIn.route) {
            GoogleSignInScreen(
                navController = navController,
                authManager = authManager
            )
        }
        composable(
            route = "add_device?domain={domain}",
            arguments = listOf(navArgument("domain") {
                type = NavType.StringType
                defaultValue = ""
            })
        ) { backStackEntry ->
            val domain = backStackEntry.arguments?.getString("domain") ?: ""
            AddDeviceScreen(
                navController = navController,
                prefillDomain = domain
            )
        }
        composable(Screen.Provision.route) {
            ProvisionScreen(navController = navController)
        }
        composable(
            route = Screen.Home.route,
            arguments = listOf(navArgument("tab") { type = NavType.IntType; defaultValue = 0 })
        ) {
            HomeScreen(navController = navController)
        }
        composable(Screen.Leds.route) {
            LedScreen(navController = navController)
        }
        composable(Screen.Temperature.route) {
            TempScreen(navController = navController)
        }
        composable(Screen.Automations.route) {
            AutomationsScreen(navController = navController)
        }
        composable(Screen.Settings.route) {
            SettingsScreen(navController = navController)
        }
    }
}
