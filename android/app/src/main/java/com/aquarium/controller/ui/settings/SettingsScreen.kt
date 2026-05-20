package com.aquarium.controller.ui.settings

import android.content.Intent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Logout
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.data.model.DuckDnsRequest
import com.aquarium.controller.data.model.MdnsRequest
import com.aquarium.controller.data.model.TelegramRequest
import com.aquarium.controller.ui.nav.Screen
import java.io.File

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    navController: NavController,
    viewModel: SettingsViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }
    val snackMsg by viewModel.snackbarMessage.collectAsState()
    val navigateToConnect by viewModel.navigateToConnect.collectAsState()
    val exportConfigData by viewModel.configExportData.collectAsState()
    var showFactoryResetDialog by remember { mutableStateOf(false) }
    val context = LocalContext.current

    LaunchedEffect(exportConfigData) {
        val bytes = exportConfigData ?: return@LaunchedEffect
        try {
            val file = File(context.cacheDir, "aquarium_config.json")
            file.writeBytes(bytes)
            val uri = FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                file
            )
            val intent = Intent(Intent.ACTION_SEND).apply {
                type = "application/json"
                putExtra(Intent.EXTRA_STREAM, uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            context.startActivity(Intent.createChooser(intent, "Export Config"))
        } catch (_: Exception) {
            snackbarHostState.showSnackbar("Export failed")
        } finally {
            viewModel.clearExportConfig()
        }
    }

    LaunchedEffect(navigateToConnect) {
        if (navigateToConnect) {
            viewModel.clearNavigation()
            navController.navigate(Screen.Connect.route) {
                popUpTo(0) { inclusive = true }
            }
        }
    }

    LaunchedEffect(snackMsg) {
        snackMsg?.let {
            snackbarHostState.showSnackbar(it)
            viewModel.clearSnackbar()
        }
    }

    if (showFactoryResetDialog) {
        AlertDialog(
            onDismissRequest = { showFactoryResetDialog = false },
            title = { Text("Factory Reset") },
            text = { Text("This will reset all settings to factory defaults. Are you sure?") },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.factoryReset()
                    showFactoryResetDialog = false
                }) { Text("Reset", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = {
                TextButton(onClick = { showFactoryResetDialog = false }) { Text("Cancel") }
            }
        )
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = { navController.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface),
                actions = {
                    IconButton(onClick = { viewModel.logout() }) {
                        Icon(Icons.AutoMirrored.Filled.Logout, contentDescription = "Logout")
                    }
                }
            )
        }
    ) { padding ->
        when (val state = uiState) {
            is SettingsUiState.Loading -> Box(
                Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center
            ) { CircularProgressIndicator() }

            is SettingsUiState.Error -> Box(
                Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(state.message, color = MaterialTheme.colorScheme.error)
                    Spacer(Modifier.height(16.dp))
                    Button(onClick = { viewModel.load() }) { Text("Retry") }
                }
            }

            is SettingsUiState.Success -> {
                val data = state.data
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding)
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // Telegram section
                    TelegramSection(
                        telegram = data.telegram,
                        onUpdate = { viewModel.updateTelegram(it) },
                        onTest = { viewModel.telegramTest() }
                    )

                    HorizontalDivider()

                    // DuckDNS section
                    DuckDnsSection(
                        duckDns = data.duckDns,
                        onUpdate = { viewModel.updateDuckDns(it) },
                        onForceUpdate = { viewModel.duckDnsUpdate() }
                    )

                    HorizontalDivider()

                    // OTA section
                    OtaSection(
                        otaStatus = data.otaStatus,
                        onStartOta = { viewModel.startOta(it) }
                    )

                    HorizontalDivider()

                    // Timezone section
                    TimezoneSection(
                        timezone = data.timezone,
                        onUpdate = { viewModel.updateTimezone(it) }
                    )

                    HorizontalDivider()

                    // mDNS section
                    MdnsSection(
                        mdns = data.mdns,
                        onUpdate = { viewModel.updateMdns(it) }
                    )

                    HorizontalDivider()

                    // Auth change section
                    AuthSection(onChangeAuth = { u, p -> viewModel.changeAuth(u, p) })

                    HorizontalDivider()

                    // Config export/import
                    Text("Configuration", style = MaterialTheme.typography.titleMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(
                            onClick = { viewModel.exportConfig() },
                            modifier = Modifier.weight(1f)
                        ) {
                            Icon(Icons.Default.Upload, contentDescription = null)
                            Spacer(Modifier.width(4.dp))
                            Text("Export")
                        }
                    }

                    HorizontalDivider()

                    // Factory reset
                    Button(
                        onClick = { showFactoryResetDialog = true },
                        modifier = Modifier.fillMaxWidth(),
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                    ) {
                        Icon(Icons.Default.RestartAlt, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text("Factory Reset")
                    }

                    // Logout
                    OutlinedButton(
                        onClick = { viewModel.logout() },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.AutoMirrored.Filled.Logout, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text("Logout")
                    }
                }
            }
        }
    }
}

@Composable
private fun TelegramSection(
    telegram: com.aquarium.controller.data.model.TelegramResponse,
    onUpdate: (TelegramRequest) -> Unit,
    onTest: () -> Unit
) {
    var botToken by remember { mutableStateOf("") }
    var chatId by remember(telegram.chatId) { mutableStateOf(telegram.chatId) }

    Column {
        Text("Telegram Notifications", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Enabled", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = telegram.enabled, onCheckedChange = { onUpdate(TelegramRequest(enabled = it)) })
        }
        Text(
            if (telegram.botTokenSet) "Bot token: set" else "Bot token: not set",
            style = MaterialTheme.typography.bodyMedium
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = botToken,
            onValueChange = { botToken = it },
            label = { Text("Bot Token") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation()
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = chatId,
            onValueChange = { chatId = it },
            label = { Text("Chat ID") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = { onUpdate(TelegramRequest(botToken = botToken.ifBlank { null }, chatId = chatId.ifBlank { null })) },
                modifier = Modifier.weight(1f)
            ) { Text("Save") }
            OutlinedButton(onClick = onTest, modifier = Modifier.weight(1f)) { Text("Test") }
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Temp alarm", style = MaterialTheme.typography.bodyMedium)
            Switch(checked = telegram.tempAlarmEnabled, onCheckedChange = { onUpdate(TelegramRequest(tempAlarmEnabled = it)) })
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Daily summary", style = MaterialTheme.typography.bodyMedium)
            Switch(checked = telegram.dailySummaryEnabled, onCheckedChange = { onUpdate(TelegramRequest(dailySummaryEnabled = it)) })
        }
    }
}

@Composable
private fun DuckDnsSection(
    duckDns: com.aquarium.controller.data.model.DuckDnsResponse,
    onUpdate: (DuckDnsRequest) -> Unit,
    onForceUpdate: () -> Unit
) {
    var domain by remember(duckDns.domain) { mutableStateOf(duckDns.domain) }
    var token by remember { mutableStateOf("") }

    Column {
        Text("DuckDNS", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Enabled", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = duckDns.enabled, onCheckedChange = { onUpdate(DuckDnsRequest(enabled = it)) })
        }
        Text("Status: ${duckDns.lastStatus}", style = MaterialTheme.typography.bodyMedium)
        Text(
            if (duckDns.tokenSet) "Token: set" else "Token: not set",
            style = MaterialTheme.typography.bodyMedium
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = domain,
            onValueChange = { domain = it },
            label = { Text("Domain") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = token,
            onValueChange = { token = it },
            label = { Text("Token") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation()
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = { onUpdate(DuckDnsRequest(domain = domain.ifBlank { null }, token = token.ifBlank { null })) },
                modifier = Modifier.weight(1f)
            ) { Text("Save") }
            OutlinedButton(onClick = onForceUpdate, modifier = Modifier.weight(1f)) { Text("Update Now") }
        }
    }
}

@Composable
private fun OtaSection(
    otaStatus: com.aquarium.controller.data.model.OtaStatusResponse,
    onStartOta: (String) -> Unit
) {
    var otaUrl by remember { mutableStateOf("") }

    Column {
        Text("OTA Update", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Text("Status: ${otaStatus.status}", style = MaterialTheme.typography.bodyMedium)
        if (otaStatus.status == "in_progress") {
            LinearProgressIndicator(
                progress = { otaStatus.progress / 100f },
                modifier = Modifier.fillMaxWidth()
            )
        }
        if (otaStatus.error.isNotBlank()) {
            Text("Error: ${otaStatus.error}", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium)
        }
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = otaUrl,
            onValueChange = { otaUrl = it },
            label = { Text("Firmware URL") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true
        )
        Spacer(Modifier.height(8.dp))
        Button(
            onClick = { if (otaUrl.isNotBlank()) onStartOta(otaUrl) },
            modifier = Modifier.fillMaxWidth()
        ) {
            Icon(Icons.Default.SystemUpdate, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Start OTA Update")
        }
    }
}

@Composable
private fun TimezoneSection(
    timezone: com.aquarium.controller.data.model.TimezoneResponse,
    onUpdate: (String) -> Unit
) {
    var tz by remember(timezone.tz) { mutableStateOf(timezone.tz) }

    Column {
        Text("Timezone", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = tz,
            onValueChange = { tz = it },
            label = { Text("TZ String (POSIX)") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            trailingIcon = {
                IconButton(onClick = { onUpdate(tz) }) {
                    Icon(Icons.Default.Check, contentDescription = "Apply")
                }
            }
        )
    }
}

@Composable
private fun MdnsSection(
    mdns: com.aquarium.controller.data.model.MdnsResponse,
    onUpdate: (MdnsRequest) -> Unit
) {
    var hostname by remember(mdns.hostname) { mutableStateOf(mdns.hostname) }

    Column {
        Text("mDNS", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Enabled", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = mdns.enabled, onCheckedChange = { onUpdate(MdnsRequest(enabled = it)) })
        }
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = hostname,
            onValueChange = { hostname = it },
            label = { Text("Hostname") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            trailingIcon = {
                IconButton(onClick = { onUpdate(MdnsRequest(hostname = hostname)) }) {
                    Icon(Icons.Default.Check, contentDescription = "Apply")
                }
            }
        )
    }
}

@Composable
private fun AuthSection(onChangeAuth: (String, String) -> Unit) {
    var username by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var confirmPassword by remember { mutableStateOf("") }

    Column {
        Text("Change Credentials", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = username,
            onValueChange = { username = it },
            label = { Text("New Username") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = password,
            onValueChange = { password = it },
            label = { Text("New Password") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation()
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = confirmPassword,
            onValueChange = { confirmPassword = it },
            label = { Text("Confirm Password") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            isError = confirmPassword.isNotBlank() && confirmPassword != password
        )
        Spacer(Modifier.height(8.dp))
        Button(
            onClick = {
                if (username.isNotBlank() && password.isNotBlank() && password == confirmPassword) {
                    onChangeAuth(username, password)
                }
            },
            modifier = Modifier.fillMaxWidth(),
            enabled = username.isNotBlank() && password.isNotBlank() && password == confirmPassword
        ) {
            Icon(Icons.Default.Key, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Update Credentials")
        }
    }
}

