package com.aquarium.controller.ui.wizard

/** All mutable state for the multi-step setup wizard. */
data class SetupWizardState(
    val currentStep: Int = 0,

    // Step 0 – connectivity check
    val isCheckingConnection: Boolean = false,
    val connectionOk: Boolean = false,

    // Step 1 – WiFi
    val networks: List<WifiNetwork> = emptyList(),
    val isScanning: Boolean = false,
    val scanError: Boolean = false,
    val selectedSsid: String = "",
    val wifiPassword: String = "",

    // Step 2 – Telegram (optional)
    val telegramToken: String = "",
    val telegramChatId: String = "",

    // Step 3 – DuckDNS
    val duckdnsDomain: String = "",
    val duckdnsToken: String = "",
    val lanPort: String = "443",

    // Step 4 – Aquarium type
    val aquariumType: AquariumType = AquariumType.TROPICAL,

    // Step 5 – send / progress
    val isSending: Boolean = false,
    val configSent: Boolean = false,
    val waitingForReconnect: Boolean = false,
    val pollingAttempt: Int = 0,
    val pollingSuccess: Boolean = false,
    val pollingFailed: Boolean = false,

    // Global error
    val error: String? = null,
)

data class WifiNetwork(
    val ssid: String,
    val rssi: Int,
    val open: Boolean,
)

enum class AquariumType(val apiValue: String, val label: String, val description: String) {
    TROPICAL("tropical", "🌴 Tropicale", "26°C · Luci 08:00–18:00 · CO2 disattiva"),
    MARINE("marine", "🐠 Marino", "25°C · Luci 08:00–20:00 con pausa · CO2 disattiva"),
    PLANTED("planted", "🌿 Piantato", "24°C · Luci 09:00–17:00 · CO2 attiva"),
}

const val WIZARD_TOTAL_STEPS = 6
