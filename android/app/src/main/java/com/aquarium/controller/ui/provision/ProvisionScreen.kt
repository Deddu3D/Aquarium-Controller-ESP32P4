package com.aquarium.controller.ui.provision

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.data.model.WifiNetworkInfo
import com.aquarium.controller.ui.nav.Screen

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ProvisionScreen(
    navController: NavController,
    viewModel: ProvisionViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }

    LaunchedEffect(uiState.navigateToLogin) {
        if (uiState.navigateToLogin) {
            viewModel.clearNavigation()
            navController.navigate(Screen.Login.route) {
                popUpTo(Screen.Provision.route) { inclusive = true }
            }
        }
    }

    LaunchedEffect(uiState.error) {
        uiState.error?.let {
            snackbarHostState.showSnackbar(it)
            viewModel.clearError()
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                title = { Text("Prima Configurazione") },
                navigationIcon = {
                    IconButton(onClick = { navController.popBackStack() }) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Indietro")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            // Progress indicator
            StepIndicator(
                currentStep = uiState.step,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp)
            )

            when (uiState.step) {
                ProvisionStep.CONNECT_TO_AP -> ConnectToApStep(
                    onOpenWifi = { viewModel.openWifiSettings() },
                    onNext = { viewModel.advanceFromConnectAp() }
                )

                ProvisionStep.PICK_WIFI -> PickWifiStep(
                    networks = uiState.networks,
                    isScanning = uiState.isScanning,
                    selectedSsid = uiState.selectedSsid,
                    password = uiState.password,
                    mdnsHostname = uiState.mdnsHostname,
                    onSelectSsid = { viewModel.updateSelectedSsid(it) },
                    onPasswordChange = { viewModel.updatePassword(it) },
                    onMdnsChange = { viewModel.updateMdns(it) },
                    onRescan = { viewModel.scanNetworks() },
                    onNext = { viewModel.applyCredentials() }
                )

                ProvisionStep.APPLYING -> ApplyingStep()

                ProvisionStep.RECONNECT -> ReconnectStep(
                    mdnsHostname = uiState.mdnsHostname,
                    isLoading = uiState.isLoading,
                    onOpenWifi = { viewModel.openWifiSettings() },
                    onVerify = { viewModel.verifyConnection() }
                )

                ProvisionStep.SERVICES -> ServicesStep(
                    telegramToken = uiState.telegramToken,
                    telegramChatId = uiState.telegramChatId,
                    duckDnsDomain = uiState.duckDnsDomain,
                    duckDnsToken = uiState.duckDnsToken,
                    deviceId = uiState.deviceId,
                    mqttEnabled = uiState.mqttEnabled,
                    isSaving = uiState.servicesSaving,
                    onTelegramTokenChange = { viewModel.updateTelegramToken(it) },
                    onTelegramChatIdChange = { viewModel.updateTelegramChatId(it) },
                    onDuckDnsDomainChange = { viewModel.updateDuckDnsDomain(it) },
                    onDuckDnsTokenChange = { viewModel.updateDuckDnsToken(it) },
                    onMqttEnabledChange = { viewModel.updateMqttEnabled(it) },
                    onSave = { viewModel.saveServicesAndFinish() },
                    onSkip = { viewModel.skipServices() }
                )

                ProvisionStep.COMPLETE -> {} // handled by LaunchedEffect
            }
        }
    }
}

// ── Step progress indicator ───────────────────────────────────────────

@Composable
private fun StepIndicator(currentStep: ProvisionStep, modifier: Modifier = Modifier) {
    val steps = listOf("WiFi AP", "Rete", "Config.", "Verifica", "Servizi")
    val currentIndex = when (currentStep) {
        ProvisionStep.CONNECT_TO_AP -> 0
        ProvisionStep.PICK_WIFI     -> 1
        ProvisionStep.APPLYING      -> 2
        ProvisionStep.RECONNECT     -> 3
        ProvisionStep.SERVICES      -> 4
        ProvisionStep.COMPLETE      -> 4
    }
    Row(
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        steps.forEachIndexed { index, label ->
            val active = index == currentIndex
            val done   = index < currentIndex
            Column(
                modifier = Modifier.weight(1f),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Surface(
                    shape = MaterialTheme.shapes.small,
                    color = when {
                        done   -> MaterialTheme.colorScheme.primary.copy(alpha = 0.4f)
                        active -> MaterialTheme.colorScheme.primary
                        else   -> MaterialTheme.colorScheme.surfaceVariant
                    },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(4.dp)
                ) {}
                Spacer(Modifier.height(2.dp))
                Text(
                    label,
                    style = MaterialTheme.typography.labelSmall,
                    color = if (active) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

// ── Step 1: Connect to AP ─────────────────────────────────────────────

@Composable
private fun ConnectToApStep(
    onOpenWifi: () -> Unit,
    onNext: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(16.dp))
        Icon(
            imageVector = Icons.Default.WifiFind,
            contentDescription = null,
            modifier = Modifier.size(72.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Text("Connettiti alla rete del controller", style = MaterialTheme.typography.headlineSmall)

        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                InstructionRow(Icons.Default.Power, "Accendi il controller Aquarium.")
                InstructionRow(Icons.Default.Wifi, "Il dispositivo creerà una rete WiFi chiamata:")
                Text(
                    "  AquariumSetup",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary
                )
                InstructionRow(Icons.Default.PhoneAndroid, "Connetti questo telefono a quella rete WiFi.")
            }
        }

        Button(onClick = onOpenWifi, modifier = Modifier.fillMaxWidth()) {
            Icon(Icons.Default.Settings, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Apri Impostazioni WiFi")
        }

        OutlinedButton(onClick = onNext, modifier = Modifier.fillMaxWidth()) {
            Text("Sono connesso ad AquariumSetup → Avanti")
        }
    }
}

@Composable
private fun InstructionRow(icon: ImageVector, text: String) {
    Row(verticalAlignment = Alignment.Top, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary)
        Text(text, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
    }
}

// ── Step 2: Pick WiFi network ─────────────────────────────────────────

@Composable
private fun PickWifiStep(
    networks: List<WifiNetworkInfo>,
    isScanning: Boolean,
    selectedSsid: String,
    password: String,
    mdnsHostname: String,
    onSelectSsid: (String) -> Unit,
    onPasswordChange: (String) -> Unit,
    onMdnsChange: (String) -> Unit,
    onRescan: () -> Unit,
    onNext: () -> Unit
) {
    var passwordVisible by remember { mutableStateOf(false) }
    val selectedNetwork = networks.find { it.ssid == selectedSsid }
    val needsPassword = selectedNetwork?.open == false || selectedSsid.isNotBlank() && selectedNetwork == null

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text("Seleziona la rete di casa", style = MaterialTheme.typography.titleMedium)
            if (isScanning) {
                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
            } else {
                IconButton(onClick = onRescan) {
                    Icon(Icons.Default.Refresh, contentDescription = "Ricarica")
                }
            }
        }

        if (networks.isNotEmpty()) {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                LazyColumn(modifier = Modifier.heightIn(max = 240.dp)) {
                    items(networks) { network ->
                        NetworkItem(
                            network = network,
                            selected = network.ssid == selectedSsid,
                            onClick = { onSelectSsid(network.ssid) }
                        )
                    }
                }
            }
        } else if (!isScanning) {
            Text(
                "Nessuna rete trovata. Tocca Ricarica.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        // Manual SSID entry
        OutlinedTextField(
            value = selectedSsid,
            onValueChange = onSelectSsid,
            label = { Text("SSID (nome rete)") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            placeholder = { Text("Seleziona dalla lista o digita manualmente") }
        )

        if (needsPassword) {
            OutlinedTextField(
                value = password,
                onValueChange = onPasswordChange,
                label = { Text("Password WiFi") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                visualTransformation = if (passwordVisible) VisualTransformation.None
                                       else PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                trailingIcon = {
                    IconButton(onClick = { passwordVisible = !passwordVisible }) {
                        Icon(
                            if (passwordVisible) Icons.Default.VisibilityOff else Icons.Default.Visibility,
                            contentDescription = null
                        )
                    }
                }
            )
        }

        // Advanced: mDNS hostname
        var showAdvanced by remember { mutableStateOf(false) }
        TextButton(onClick = { showAdvanced = !showAdvanced }) {
            Icon(if (showAdvanced) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                contentDescription = null, modifier = Modifier.size(16.dp))
            Spacer(Modifier.width(4.dp))
            Text("Opzioni avanzate", style = MaterialTheme.typography.labelMedium)
        }
        if (showAdvanced) {
            OutlinedTextField(
                value = mdnsHostname,
                onValueChange = onMdnsChange,
                label = { Text("Hostname mDNS") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                placeholder = { Text("aquarium") },
                supportingText = { Text("Il controller sarà raggiungibile come $mdnsHostname.local") }
            )
        }

        Button(
            onClick = onNext,
            modifier = Modifier.fillMaxWidth(),
            enabled = selectedSsid.isNotBlank()
        ) {
            Icon(Icons.Default.ArrowForward, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Configura")
        }
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun NetworkItem(
    network: WifiNetworkInfo,
    selected: Boolean,
    onClick: () -> Unit
) {
    val rssiIcon = when {
        network.rssi >= -50 -> Icons.Default.SignalWifi4Bar
        network.rssi >= -70 -> Icons.Default.NetworkWifi
        else                -> Icons.Default.SignalWifi1Bar
    }
    ListItem(
        headlineContent = { Text(network.ssid) },
        leadingContent = {
            Icon(
                rssiIcon, contentDescription = null,
                tint = if (selected) MaterialTheme.colorScheme.primary
                       else MaterialTheme.colorScheme.onSurfaceVariant
            )
        },
        trailingContent = {
            if (!network.open) {
                Icon(Icons.Default.Lock, contentDescription = "Password richiesta",
                    modifier = Modifier.size(16.dp))
            }
            if (selected) {
                Icon(Icons.Default.Check, contentDescription = "Selezionata",
                    tint = MaterialTheme.colorScheme.primary)
            }
        },
        colors = ListItemDefaults.colors(
            containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer
                             else MaterialTheme.colorScheme.surface
        ),
        modifier = Modifier.clickable(onClick = onClick)
    )
}

// ── Step 3: Applying ──────────────────────────────────────────────────

@Composable
private fun ApplyingStep() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        CircularProgressIndicator(modifier = Modifier.size(64.dp))
        Spacer(Modifier.height(24.dp))
        Text("Invio credenziali in corso…", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Text(
            "Il controller si disconnetterà dalla rete AquariumSetup. È normale.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

// ── Step 4: Reconnect and verify ──────────────────────────────────────

@Composable
private fun ReconnectStep(
    mdnsHostname: String,
    isLoading: Boolean,
    onOpenWifi: () -> Unit,
    onVerify: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(16.dp))
        Icon(
            Icons.Default.WifiProtectedSetup,
            contentDescription = null,
            modifier = Modifier.size(72.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Text("Riconnetti il telefono", style = MaterialTheme.typography.headlineSmall)

        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                InstructionRow(Icons.Default.Wifi, "Riconnetti questo telefono alla tua rete WiFi di casa.")
                InstructionRow(Icons.Default.Router, "Il controller si connetterà alla stessa rete.")
                InstructionRow(Icons.Default.Search, "Sarà raggiungibile come:")
                Text(
                    "  $mdnsHostname.local",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        Button(onClick = onOpenWifi, modifier = Modifier.fillMaxWidth()) {
            Icon(Icons.Default.Settings, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Apri Impostazioni WiFi")
        }

        Button(
            onClick = onVerify,
            modifier = Modifier.fillMaxWidth(),
            enabled = !isLoading
        ) {
            if (isLoading) {
                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
            } else {
                Icon(Icons.Default.CheckCircle, contentDescription = null)
                Spacer(Modifier.width(8.dp))
            }
            Text("Verifica connessione")
        }
    }
}

// ── Step 5: Optional services setup ──────────────────────────────────

@Composable
private fun ServicesStep(
    telegramToken: String,
    telegramChatId: String,
    duckDnsDomain: String,
    duckDnsToken: String,
    deviceId: String,
    mqttEnabled: Boolean,
    isSaving: Boolean,
    onTelegramTokenChange: (String) -> Unit,
    onTelegramChatIdChange: (String) -> Unit,
    onDuckDnsDomainChange: (String) -> Unit,
    onDuckDnsTokenChange: (String) -> Unit,
    onMqttEnabledChange: (Boolean) -> Unit,
    onSave: () -> Unit,
    onSkip: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Spacer(Modifier.height(8.dp))
        Text("Configurazione Servizi", style = MaterialTheme.typography.titleLarge)
        Text(
            "Facoltativo – puoi configurare questi servizi anche in seguito dalle Impostazioni.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        // ── Remote Access (MQTT) ──────────────────────────────────────
        HorizontalDivider()
        Text("Accesso Remoto", style = MaterialTheme.typography.titleMedium)
        if (deviceId.isNotBlank()) {
            Text(
                "Device ID del controller (salvalo per accedere da remoto):",
                style = MaterialTheme.typography.bodyMedium
            )
            OutlinedTextField(
                value = deviceId,
                onValueChange = {},
                readOnly = true,
                label = { Text("Device ID") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                trailingIcon = {
                    Icon(
                        imageVector = Icons.Default.Wifi,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary
                    )
                }
            )
        } else {
            Text(
                "Device ID non disponibile – assicurati che il controller sia raggiungibile.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text("Abilita accesso remoto MQTT", style = MaterialTheme.typography.bodyLarge)
                Text(
                    "Raggiunge il controller da qualsiasi rete senza configurare il router.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = mqttEnabled,
                onCheckedChange = onMqttEnabledChange,
                enabled = deviceId.isNotBlank()
            )
        }

        HorizontalDivider()

        // Telegram
        Text("Telegram", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = telegramToken,
            onValueChange = onTelegramTokenChange,
            label = { Text("Bot Token") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            placeholder = { Text("123456789:AAFxx...") }
        )
        OutlinedTextField(
            value = telegramChatId,
            onValueChange = onTelegramChatIdChange,
            label = { Text("Chat ID") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            placeholder = { Text("-100123456789") }
        )

        HorizontalDivider()

        // DuckDNS
        Text("DuckDNS", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = duckDnsDomain,
            onValueChange = onDuckDnsDomainChange,
            label = { Text("Dominio") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            placeholder = { Text("mio-acquario") },
            supportingText = { Text("mio-acquario.duckdns.org") }
        )
        OutlinedTextField(
            value = duckDnsToken,
            onValueChange = onDuckDnsTokenChange,
            label = { Text("Token DuckDNS") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            visualTransformation = PasswordVisualTransformation()
        )

        Spacer(Modifier.height(4.dp))
        Button(
            onClick = onSave,
            modifier = Modifier.fillMaxWidth(),
            enabled = !isSaving
        ) {
            if (isSaving) {
                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
            }
            Text("Salva e continua")
        }
        OutlinedButton(onClick = onSkip, modifier = Modifier.fillMaxWidth(), enabled = !isSaving) {
            Text("Salta – configura in seguito")
        }
        Spacer(Modifier.height(16.dp))
    }
}
