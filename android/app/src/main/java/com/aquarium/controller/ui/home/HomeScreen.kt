package com.aquarium.controller.ui.home

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.data.model.RelayInfo
import com.aquarium.controller.ui.nav.Screen

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    navController: NavController,
    viewModel: HomeViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
    val wsStatus by viewModel.wsStatus.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }
    val snackMsg by viewModel.snackbarMessage.collectAsState()

    LaunchedEffect(snackMsg) {
        snackMsg?.let {
            snackbarHostState.showSnackbar(it)
            viewModel.clearSnackbar()
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                title = { Text("Aquarium Controller") },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                ),
                actions = {
                    IconButton(onClick = { navController.navigate(Screen.Settings.route) }) {
                        Icon(Icons.Default.Settings, contentDescription = "Settings")
                    }
                }
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = { viewModel.load() }) {
                Icon(Icons.Default.Refresh, contentDescription = "Refresh")
            }
        },
        bottomBar = {
            NavigationBar(containerColor = MaterialTheme.colorScheme.surface) {
                NavigationBarItem(
                    selected = true,
                    onClick = {},
                    icon = { Icon(Icons.Default.Home, contentDescription = null) },
                    label = { Text("Home") }
                )
                NavigationBarItem(
                    selected = false,
                    onClick = {
                        navController.navigate(Screen.Leds.route) {
                            popUpTo(Screen.Home.createRoute()) { saveState = true }
                            launchSingleTop = true
                            restoreState = true
                        }
                    },
                    icon = { Icon(Icons.Default.Lightbulb, contentDescription = null) },
                    label = { Text("LEDs") }
                )
                NavigationBarItem(
                    selected = false,
                    onClick = {
                        navController.navigate(Screen.Temperature.route) {
                            popUpTo(Screen.Home.createRoute()) { saveState = true }
                            launchSingleTop = true
                            restoreState = true
                        }
                    },
                    icon = { Icon(Icons.Default.Thermostat, contentDescription = null) },
                    label = { Text("Temp") }
                )
                NavigationBarItem(
                    selected = false,
                    onClick = {
                        navController.navigate(Screen.Automations.route) {
                            popUpTo(Screen.Home.createRoute()) { saveState = true }
                            launchSingleTop = true
                            restoreState = true
                        }
                    },
                    icon = { Icon(Icons.Default.AutoMode, contentDescription = null) },
                    label = { Text("Auto") }
                )
            }
        }
    ) { padding ->
        when (val state = uiState) {
            is HomeUiState.Loading -> {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding),
                    contentAlignment = Alignment.Center
                ) {
                    CircularProgressIndicator()
                }
            }
            is HomeUiState.Error -> {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(Icons.Default.Error, contentDescription = null, tint = MaterialTheme.colorScheme.error)
                        Spacer(Modifier.height(8.dp))
                        Text(state.message, color = MaterialTheme.colorScheme.error)
                        Spacer(Modifier.height(16.dp))
                        Button(onClick = { viewModel.load() }) { Text("Retry") }
                    }
                }
            }
            is HomeUiState.Success -> {
                val displayTemp = when {
                    wsStatus.connected -> "%.1f°C".format(wsStatus.tempC)
                    else -> "--°C"
                }
                val tempOk = wsStatus.tempOk
                val tempConnected = wsStatus.connected

                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding)
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // Temperature badge (local WebSocket or remote MQTT)
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column {
                                Text("Temperature", style = MaterialTheme.typography.labelLarge)
                                Text(
                                    displayTemp,
                                    style = MaterialTheme.typography.headlineMedium,
                                    color = when {
                                        !tempConnected -> MaterialTheme.colorScheme.onSurface
                                        !tempOk -> MaterialTheme.colorScheme.error
                                        else -> MaterialTheme.colorScheme.secondary
                                    }
                                )
                            }
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Icon(
                                    imageVector = Icons.Default.Thermostat,
                                    contentDescription = null,
                                    tint = if (tempOk) MaterialTheme.colorScheme.secondary
                                           else MaterialTheme.colorScheme.error
                                )
                                Spacer(Modifier.width(8.dp))
                                // Show Wifi icon based on connection status
                                Icon(
                                    imageVector = when {
                                        tempConnected  -> Icons.Default.Wifi
                                        else           -> Icons.Default.WifiOff
                                    },
                                    contentDescription = null,
                                    tint = if (tempConnected) MaterialTheme.colorScheme.secondary
                                           else MaterialTheme.colorScheme.error
                                )
                            }
                        }
                    }

                    // LED quick control
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Icon(
                                    imageVector = Icons.Default.Lightbulb,
                                    contentDescription = null,
                                    tint = if (state.leds.on) MaterialTheme.colorScheme.secondary
                                           else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.4f)
                                )
                                Spacer(Modifier.width(8.dp))
                                Text("Lights", style = MaterialTheme.typography.titleMedium)
                            }
                            Switch(
                                checked = state.leds.on,
                                onCheckedChange = { viewModel.setLedOn(it) }
                            )
                        }
                    }

                    // Relay quick toggles
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Text("Quick Controls", style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            state.relays.relays.forEach { relay ->
                                RelayToggleRow(relay = relay, onToggle = { viewModel.toggleRelay(relay.index, relay.on) })
                            }
                        }
                    }

                    // Feeding card
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column {
                                Text("Feeding Mode", style = MaterialTheme.typography.titleMedium)
                                if (state.feeding.active) {
                                    Text(
                                        "Active - ${state.feeding.remainingS}s remaining",
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = MaterialTheme.colorScheme.secondary
                                    )
                                } else {
                                    Text("Inactive", style = MaterialTheme.typography.bodyMedium)
                                }
                            }
                            Button(
                                onClick = {
                                    if (state.feeding.active) viewModel.stopFeeding()
                                    else viewModel.startFeeding()
                                },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = if (state.feeding.active) MaterialTheme.colorScheme.error
                                    else MaterialTheme.colorScheme.primary
                                )
                            ) {
                                Text(if (state.feeding.active) "Stop" else "Start")
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun RelayToggleRow(relay: RelayInfo, onToggle: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(relay.name.ifBlank { "Relay ${relay.index}" }, style = MaterialTheme.typography.bodyLarge)
        Switch(checked = relay.on, onCheckedChange = { onToggle() })
    }
}