package com.aquarium.controller.ui.nav

import androidx.compose.runtime.Composable
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.aquarium.controller.ui.automations.AutomationsScreen
import com.aquarium.controller.ui.connect.ConnectScreen
import com.aquarium.controller.ui.home.HomeScreen
import com.aquarium.controller.ui.leds.LedScreen
import com.aquarium.controller.ui.login.LoginScreen
import com.aquarium.controller.ui.settings.SettingsScreen
import com.aquarium.controller.ui.temperature.TempScreen

sealed class Screen(val route: String) {
    object Connect : Screen("connect")
    object Login : Screen("login")
    object Home : Screen("home/{tab}") {
        fun createRoute(tab: Int = 0) = "home/$tab"
    }
    object Leds : Screen("leds")
    object Temperature : Screen("temperature")
    object Automations : Screen("automations")
    object Settings : Screen("settings")
}

@Composable
fun AppNavigation() {
    val navController = rememberNavController()
    NavHost(navController = navController, startDestination = Screen.Connect.route) {
        composable(Screen.Connect.route) {
            ConnectScreen(navController = navController)
        }
        composable(Screen.Login.route) {
            LoginScreen(navController = navController)
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
