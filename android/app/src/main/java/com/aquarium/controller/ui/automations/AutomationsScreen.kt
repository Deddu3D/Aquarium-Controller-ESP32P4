package com.aquarium.controller.ui.automations

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.data.model.Co2Request
import com.aquarium.controller.data.model.Co2Response
import com.aquarium.controller.data.model.HeaterRequest
import com.aquarium.controller.data.model.RelayInfo
import com.aquarium.controller.data.model.RelaySchedule

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AutomationsScreen(
    navController: NavController,
    viewModel: AutomationsViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
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
                title = { Text("Automations") },
                navigationIcon = {
                    IconButton(onClick = { navController.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface),
                actions = {
                    IconButton(onClick = { viewModel.load() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                    }
                }
            )
        }
    ) { padding ->
        when (val state = uiState) {
            is AutomationsUiState.Loading -> Box(
                Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center
            ) { CircularProgressIndicator() }

            is AutomationsUiState.Error -> Box(
                Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(state.message, color = MaterialTheme.colorScheme.error)
                    Spacer(Modifier.height(16.dp))
                    Button(onClick = { viewModel.load() }) { Text("Retry") }
                }
            }

            is AutomationsUiState.Success -> {
                val data = state.data
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding)
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // Relay cards
                    Text("Relays", style = MaterialTheme.typography.headlineMedium)
                    data.relays.relays.forEach { relay ->
                        RelayCard(
                            relay = relay,
                            onToggle = { viewModel.toggleRelay(relay.index, !relay.on) }
                        )
                    }

                    HorizontalDivider()

                    // CO2 card
                    Text("CO₂ Control", style = MaterialTheme.typography.headlineMedium)
                    Co2Card(
                        co2 = data.co2,
                        onUpdate = { viewModel.updateCo2(it) }
                    )

                    HorizontalDivider()

                    // Heater shortcut
                    Text("Heater", style = MaterialTheme.typography.headlineMedium)
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column {
                                    Text("Heater Control", style = MaterialTheme.typography.titleMedium)
                                    Text(
                                        "Target: %.1f°C  Hysteresis: %.1f°C".format(
                                            data.heater.targetTempC, data.heater.hysteresisC
                                        ),
                                        style = MaterialTheme.typography.bodyMedium
                                    )
                                    Text("Relay #${data.heater.relayIndex}", style = MaterialTheme.typography.bodyMedium)
                                }
                                Switch(
                                    checked = data.heater.enabled,
                                    onCheckedChange = { viewModel.updateHeater(HeaterRequest(enabled = it)) }
                                )
                            }
                        }
                    }

                    HorizontalDivider()

                    // Feeding card
                    Text("Feeding", style = MaterialTheme.typography.headlineMedium)
                    FeedingCard(
                        feeding = data.feeding,
                        onStart = { viewModel.startFeeding() },
                        onStop = { viewModel.stopFeeding() }
                    )
                }
            }
        }
    }
}

@Composable
private fun RelayCard(relay: RelayInfo, onToggle: () -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(relay.name.ifBlank { "Relay ${relay.index}" }, style = MaterialTheme.typography.titleMedium)
                    Text("Index: ${relay.index}", style = MaterialTheme.typography.bodyMedium)
                }
                Switch(checked = relay.on, onCheckedChange = { onToggle() })
            }
            if (relay.schedules.any { it.enabled }) {
                Spacer(Modifier.height(8.dp))
                Text("Schedules:", style = MaterialTheme.typography.labelLarge)
                relay.schedules.filter { it.enabled }.forEach { schedule ->
                    ScheduleRow(schedule)
                }
            }
        }
    }
}

@Composable
private fun ScheduleRow(schedule: RelaySchedule) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Icon(Icons.Default.Schedule, contentDescription = null, modifier = Modifier.size(16.dp))
        Spacer(Modifier.width(4.dp))
        Text(
            "On: %02d:%02d  Off: %02d:%02d".format(
                schedule.onMin / 60, schedule.onMin % 60,
                schedule.offMin / 60, schedule.offMin % 60
            ),
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
private fun Co2Card(co2: Co2Response, onUpdate: (Co2Request) -> Unit) {
    var preOn by remember(co2.preOnMin) { mutableStateOf(co2.preOnMin.toString()) }
    var postOff by remember(co2.postOffMin) { mutableStateOf(co2.postOffMin.toString()) }

    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("CO₂ Injection", style = MaterialTheme.typography.titleMedium)
                    Text("Relay #${co2.relayIndex}", style = MaterialTheme.typography.bodyMedium)
                }
                Switch(
                    checked = co2.enabled,
                    onCheckedChange = { onUpdate(Co2Request(enabled = it)) }
                )
            }
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = preOn,
                onValueChange = { preOn = it },
                label = { Text("Pre-on minutes") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                trailingIcon = {
                    IconButton(onClick = {
                        preOn.toIntOrNull()?.let { onUpdate(Co2Request(preOnMin = it)) }
                    }) {
                        Icon(Icons.Default.Check, contentDescription = "Apply")
                    }
                }
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = postOff,
                onValueChange = { postOff = it },
                label = { Text("Post-off minutes") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                trailingIcon = {
                    IconButton(onClick = {
                        postOff.toIntOrNull()?.let { onUpdate(Co2Request(postOffMin = it)) }
                    }) {
                        Icon(Icons.Default.Check, contentDescription = "Apply")
                    }
                }
            )
        }
    }
}

@Composable
private fun FeedingCard(
    feeding: com.aquarium.controller.data.model.FeedingResponse,
    onStart: () -> Unit,
    onStop: () -> Unit
) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("Feeding Mode", style = MaterialTheme.typography.titleMedium)
                    Text(
                        if (feeding.active) "Active — ${feeding.remainingS}s remaining"
                        else "Inactive",
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (feeding.active) MaterialTheme.colorScheme.secondary
                        else MaterialTheme.colorScheme.onSurface
                    )
                    Text("Duration: ${feeding.durationMin} min  Relay #${feeding.relayIndex}", style = MaterialTheme.typography.bodyMedium)
                }
                Button(
                    onClick = if (feeding.active) onStop else onStart,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (feeding.active) MaterialTheme.colorScheme.error
                        else MaterialTheme.colorScheme.primary
                    )
                ) {
                    Text(if (feeding.active) "Stop" else "Start")
                }
            }
        }
    }
}
