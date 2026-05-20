package com.aquarium.controller.ui.temperature

import android.content.Intent
import androidx.compose.foundation.Canvas
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.data.model.HeaterRequest
import com.aquarium.controller.data.model.TempSample
import java.io.File
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TempScreen(
    navController: NavController,
    viewModel: TempViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
    val wsStatus by viewModel.wsStatus.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }
    val snackMsg by viewModel.snackbarMessage.collectAsState()
    val exportData by viewModel.exportData.collectAsState()
    val context = LocalContext.current

    LaunchedEffect(exportData) {
        val csv = exportData ?: return@LaunchedEffect
        try {
            val file = File(context.cacheDir, "temperature_history.csv")
            file.writeText(csv)
            val uri = FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                file
            )
            val intent = Intent(Intent.ACTION_SEND).apply {
                type = "text/csv"
                putExtra(Intent.EXTRA_STREAM, uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            context.startActivity(Intent.createChooser(intent, "Export CSV"))
        } catch (_: Exception) {
            snackbarHostState.showSnackbar("Export failed")
        } finally {
            viewModel.clearExportData()
        }
    }

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
                title = { Text("Temperature") },
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
            is TempUiState.Loading -> Box(
                Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center
            ) { CircularProgressIndicator() }

            is TempUiState.Error -> Box(
                Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(state.message, color = MaterialTheme.colorScheme.error)
                    Spacer(Modifier.height(16.dp))
                    Button(onClick = { viewModel.load() }) { Text("Retry") }
                }
            }

            is TempUiState.Success -> {
                val data = state.data
                var targetTemp by remember(data.heater.targetTempC) { mutableStateOf(data.heater.targetTempC.toString()) }
                var hysteresis by remember(data.heater.hysteresisC) { mutableStateOf(data.heater.hysteresisC.toString()) }

                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding)
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // Current temp card
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Row(
                            modifier = Modifier.fillMaxWidth().padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column {
                                Text("Current Temperature", style = MaterialTheme.typography.labelLarge)
                                Text(
                                    if (wsStatus.connected) "%.1f°C".format(wsStatus.tempC)
                                    else if (data.current.valid) "%.1f°C".format(data.current.temperatureC)
                                    else "--°C",
                                    style = MaterialTheme.typography.headlineLarge,
                                    color = when {
                                        wsStatus.connected && (wsStatus.tempC > 28.0 || wsStatus.tempC < 22.0) -> MaterialTheme.colorScheme.error
                                        else -> MaterialTheme.colorScheme.primary
                                    }
                                )
                                Text(
                                    if (data.current.valid) "Sensor OK" else "Sensor error",
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = if (data.current.valid) MaterialTheme.colorScheme.secondary else MaterialTheme.colorScheme.error
                                )
                            }
                            Icon(
                                imageVector = Icons.Default.Thermostat,
                                contentDescription = null,
                                modifier = Modifier.size(48.dp),
                                tint = MaterialTheme.colorScheme.primary
                            )
                        }
                    }

                    // Chart
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Text("History (last ${data.history.count} samples)", style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            TempLineChart(samples = data.history.samples, modifier = Modifier.fillMaxWidth().height(200.dp))
                        }
                    }

                    // Export CSV button
                    OutlinedButton(
                        onClick = { viewModel.exportCsv() },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Default.Download, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text("Export CSV")
                    }

                    // Heater card
                    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("Heater", style = MaterialTheme.typography.titleMedium)
                                Switch(
                                    checked = data.heater.enabled,
                                    onCheckedChange = { viewModel.updateHeater(HeaterRequest(enabled = it)) }
                                )
                            }
                            Spacer(Modifier.height(8.dp))
                            Text("Relay: #${data.heater.relayIndex}", style = MaterialTheme.typography.bodyMedium)
                            Spacer(Modifier.height(8.dp))
                            OutlinedTextField(
                                value = targetTemp,
                                onValueChange = { targetTemp = it },
                                label = { Text("Target Temperature (°C)") },
                                modifier = Modifier.fillMaxWidth(),
                                singleLine = true,
                                trailingIcon = {
                                    IconButton(onClick = {
                                        targetTemp.toDoubleOrNull()?.let {
                                            viewModel.updateHeater(HeaterRequest(targetTempC = it))
                                        }
                                    }) {
                                        Icon(Icons.Default.Check, contentDescription = "Apply")
                                    }
                                }
                            )
                            Spacer(Modifier.height(8.dp))
                            OutlinedTextField(
                                value = hysteresis,
                                onValueChange = { hysteresis = it },
                                label = { Text("Hysteresis (°C)") },
                                modifier = Modifier.fillMaxWidth(),
                                singleLine = true,
                                trailingIcon = {
                                    IconButton(onClick = {
                                        hysteresis.toDoubleOrNull()?.let {
                                            viewModel.updateHeater(HeaterRequest(hysteresisC = it))
                                        }
                                    }) {
                                        Icon(Icons.Default.Check, contentDescription = "Apply")
                                    }
                                }
                            )
                            Spacer(Modifier.height(8.dp))
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("Runaway Protection", style = MaterialTheme.typography.bodyMedium)
                                Switch(
                                    checked = data.heater.runawayProtection,
                                    onCheckedChange = { viewModel.updateHeater(HeaterRequest(runawayProtection = it)) }
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun TempLineChart(samples: List<TempSample>, modifier: Modifier = Modifier) {
    val lineColor = Color(0xFF1FA3FF)
    val gridColor = Color(0x33FFFFFF)
    val labelColor = Color(0xAAFFFFFF)
    val textMeasurer = rememberTextMeasurer()
    val labelStyle = TextStyle(fontSize = 10.sp, color = labelColor)
    val formatter = remember { DateTimeFormatter.ofPattern("HH:mm").withZone(ZoneId.systemDefault()) }

    Canvas(modifier = modifier) {
        if (samples.size < 2) return@Canvas

        val padLeft = 48.dp.toPx()
        val padBottom = 24.dp.toPx()
        val padTop = 8.dp.toPx()
        val padRight = 8.dp.toPx()
        val chartW = size.width - padLeft - padRight
        val chartH = size.height - padBottom - padTop

        val minY = samples.minOf { it.c }.toFloat() - 0.5f
        val maxY = samples.maxOf { it.c }.toFloat() + 0.5f
        val rangeY = (maxY - minY).coerceAtLeast(0.1f)

        fun toX(i: Int) = padLeft + i.toFloat() / (samples.size - 1) * chartW
        fun toY(v: Float) = padTop + (1f - (v - minY) / rangeY) * chartH

        // Grid lines (3 horizontal)
        for (step in 0..2) {
            val gy = padTop + step * chartH / 2
            drawLine(gridColor, Offset(padLeft, gy), Offset(padLeft + chartW, gy), strokeWidth = 1f)
            val value = maxY - step * rangeY / 2
            val label = "%.1f".format(value)
            val measured = textMeasurer.measure(label, labelStyle)
            drawText(measured, topLeft = Offset(0f, gy - measured.size.height / 2))
        }

        // Line path
        val path = Path()
        samples.forEachIndexed { i, s ->
            val x = toX(i)
            val y = toY(s.c.toFloat())
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        drawPath(path, lineColor, style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round))

        // X labels (first, middle, last)
        listOf(0, samples.size / 2, samples.size - 1).forEach { idx ->
            val label = formatter.format(Instant.ofEpochSecond(samples[idx].t))
            val measured = textMeasurer.measure(label, labelStyle)
            val lx = toX(idx) - measured.size.width / 2
            drawText(measured, topLeft = Offset(lx.coerceIn(padLeft, padLeft + chartW - measured.size.width),
                padTop + chartH + 4.dp.toPx()))
        }
    }
}
