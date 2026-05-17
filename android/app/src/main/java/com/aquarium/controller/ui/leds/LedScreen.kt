package com.aquarium.controller.ui.leds

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.data.model.LedScheduleRequest

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun LedScreen(
    navController: NavController,
    viewModel: LedViewModel = hiltViewModel()
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
                title = { Text("LED Control") },
                navigationIcon = {
                    IconButton(onClick = { navController.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface)
            )
        }
    ) { padding ->
        when (val state = uiState) {
            is LedUiState.Loading -> Box(
                Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center
            ) { CircularProgressIndicator() }

            is LedUiState.Error -> Box(
                Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(state.message, color = MaterialTheme.colorScheme.error)
                    Spacer(Modifier.height(16.dp))
                    Button(onClick = { viewModel.load() }) { Text("Retry") }
                }
            }

            is LedUiState.Success -> {
                val data = state.data
                var localBrightness by remember(data.leds.brightness) { mutableStateOf(data.leds.brightness.toFloat()) }
                var localR by remember(data.leds.r) { mutableStateOf(data.leds.r.toFloat()) }
                var localG by remember(data.leds.g) { mutableStateOf(data.leds.g.toFloat()) }
                var localB by remember(data.leds.b) { mutableStateOf(data.leds.b.toFloat()) }

                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding)
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // On/Off + Brightness
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("LED Strip", style = MaterialTheme.typography.titleMedium)
                                Switch(checked = data.leds.on, onCheckedChange = { viewModel.setOn(it) })
                            }
                            Spacer(Modifier.height(8.dp))
                            Text("Brightness: ${localBrightness.toInt()}", style = MaterialTheme.typography.bodyMedium)
                            Slider(
                                value = localBrightness,
                                onValueChange = { localBrightness = it },
                                onValueChangeFinished = { viewModel.setBrightness(localBrightness.toInt()) },
                                valueRange = 0f..255f,
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    }

                    // RGB Sliders
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Text("Color", style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            Box(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .height(32.dp),
                            ) {
                                Surface(
                                    modifier = Modifier.fillMaxSize(),
                                    color = Color(localR.toInt(), localG.toInt(), localB.toInt()),
                                    shape = MaterialTheme.shapes.small
                                ) {}
                            }
                            Spacer(Modifier.height(8.dp))
                            RgbSlider("R", localR, Color.Red) {
                                localR = it
                                viewModel.setColor(localR.toInt(), localG.toInt(), localB.toInt())
                            }
                            RgbSlider("G", localG, Color.Green) {
                                localG = it
                                viewModel.setColor(localR.toInt(), localG.toInt(), localB.toInt())
                            }
                            RgbSlider("B", localB, Color.Blue) {
                                localB = it
                                viewModel.setColor(localR.toInt(), localG.toInt(), localB.toInt())
                            }
                        }
                    }

                    // Scene chips
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Text("Scene", style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            val scenes = listOf("NONE", "SUNRISE", "SUNSET", "MOONLIGHT", "STORM", "CLOUDS")
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                scenes.forEachIndexed { index, name ->
                                    FilterChip(
                                        selected = data.scene.active == index,
                                        onClick = { viewModel.startScene(index) },
                                        label = { Text(name) }
                                    )
                                }
                            }
                        }
                    }

                    // Schedule card
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("Schedule", style = MaterialTheme.typography.titleMedium)
                                Switch(
                                    checked = data.schedule.enabled,
                                    onCheckedChange = { viewModel.updateSchedule(LedScheduleRequest(enabled = it)) }
                                )
                            }
                            if (data.schedule.enabled) {
                                Spacer(Modifier.height(8.dp))
                                Text("On: %02d:%02d  Off: %02d:%02d".format(
                                    data.schedule.onHour, data.schedule.onMinute,
                                    data.schedule.offHour, data.schedule.offMinute
                                ), style = MaterialTheme.typography.bodyMedium)
                                Text("Ramp: ${data.schedule.rampDurationMin} min", style = MaterialTheme.typography.bodyMedium)
                            }
                        }
                    }

                    // Presets
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Text("Presets", style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            data.presets.presets.forEach { preset ->
                                Row(
                                    modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Text(
                                        preset.name.ifBlank { "Slot ${preset.slot}" },
                                        style = MaterialTheme.typography.bodyMedium,
                                        modifier = Modifier.weight(1f)
                                    )
                                    TextButton(onClick = { viewModel.loadPreset(preset.slot) }) {
                                        Text("Load")
                                    }
                                    TextButton(onClick = { viewModel.savePreset(preset.slot, preset.name.ifBlank { "Preset ${preset.slot}" }) }) {
                                        Text("Save")
                                    }
                                }
                            }
                        }
                    }

                    // Daily Cycle
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Row(
                            modifier = Modifier.fillMaxWidth().padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column {
                                Text("Daily Cycle", style = MaterialTheme.typography.titleMedium)
                                Text(
                                    "Phase: ${data.dailyCycle.phase}  " +
                                    "Sunrise: %02d:%02d  Sunset: %02d:%02d".format(
                                        data.dailyCycle.sunriseMin / 60, data.dailyCycle.sunriseMin % 60,
                                        data.dailyCycle.sunsetMin / 60, data.dailyCycle.sunsetMin % 60
                                    ),
                                    style = MaterialTheme.typography.bodyMedium
                                )
                            }
                            Switch(
                                checked = data.dailyCycle.enabled,
                                onCheckedChange = { viewModel.setDailyCycleEnabled(it) }
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun RgbSlider(
    label: String,
    value: Float,
    color: Color,
    onValueChangeFinished: (Float) -> Unit
) {
    var localValue by remember(value) { mutableStateOf(value) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            "$label: ${localValue.toInt()}",
            modifier = Modifier.width(60.dp),
            style = MaterialTheme.typography.bodyMedium
        )
        Slider(
            value = localValue,
            onValueChange = { localValue = it },
            onValueChangeFinished = { onValueChangeFinished(localValue) },
            valueRange = 0f..255f,
            modifier = Modifier.fillMaxWidth(),
            colors = SliderDefaults.colors(thumbColor = color, activeTrackColor = color)
        )
    }
}
