package com.aquarium.controller.ui.wizard

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SetupWizardScreen(
    onSetupComplete: (String) -> Unit,
    viewModel: SetupWizardViewModel = viewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Passo ${state.currentStep + 1} di $WIZARD_TOTAL_STEPS") },
                navigationIcon = {
                    if (state.currentStep > 0 && !state.configSent) {
                        IconButton(onClick = { viewModel.prevStep() }) {
                            Icon(Icons.Default.ArrowBack, contentDescription = "Indietro")
                        }
                    }
                },
            )
        }
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            when (state.currentStep) {
                0 -> Step0Connect(state, viewModel)
                1 -> Step1Wifi(state, viewModel)
                2 -> Step2Telegram(state, viewModel)
                3 -> Step3DuckDns(state, viewModel)
                4 -> Step4Profile(state, viewModel)
                5 -> Step5Summary(state, viewModel, onSetupComplete)
            }
        }
    }
}

// ── Step 0: Connect to AP ─────────────────────────────────────────────────────

@Composable
private fun Step0Connect(state: SetupWizardState, vm: SetupWizardViewModel) {
    Column(
        Modifier.fillMaxSize().padding(24.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("Connetti all'ESP", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text(
            "1. Apri le Impostazioni WiFi del telefono.\n" +
            "2. Connettiti alla rete «AquariumSetup».\n" +
            "3. Torna qui e tocca Controlla connessione.",
            style = MaterialTheme.typography.bodyMedium,
        )
        if (state.error != null) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
        }
        if (state.connectionOk) {
            Text("✅ Connesso all'ESP", color = MaterialTheme.colorScheme.primary)
        }
        Button(
            onClick = { vm.checkConnection() },
            enabled = !state.isCheckingConnection,
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (state.isCheckingConnection) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
            }
            Text(if (state.isCheckingConnection) "Verifica in corso…" else "Controlla connessione")
        }
        if (state.connectionOk) {
            Button(onClick = { vm.nextStep() }, modifier = Modifier.fillMaxWidth()) {
                Text("Avanti")
            }
        }
    }
}

// ── Step 1: WiFi ──────────────────────────────────────────────────────────────

@Composable
private fun Step1Wifi(state: SetupWizardState, vm: SetupWizardViewModel) {
    LaunchedEffect(Unit) { if (state.networks.isEmpty()) vm.scanNetworks() }

    Column(
        Modifier.fillMaxSize().padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Rete WiFi", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text("Seleziona la rete a cui collegare l'ESP.")

        Row(verticalAlignment = Alignment.CenterVertically) {
            if (state.isScanning) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
                Text("Scansione reti…", style = MaterialTheme.typography.labelMedium)
            } else {
                TextButton(onClick = { vm.scanNetworks() }) { Text("Aggiorna lista") }
            }
        }

        if (state.networks.isNotEmpty()) {
            LazyColumn(Modifier.weight(1f)) {
                items(state.networks) { net ->
                    NetworkRow(net, selected = net.ssid == state.selectedSsid) {
                        vm.onSsidChange(net.ssid)
                    }
                }
            }
        } else if (state.scanError) {
            Text("Impossibile ottenere la lista reti. Inserisci l'SSID manualmente.", color = MaterialTheme.colorScheme.error)
        }

        OutlinedTextField(
            value = state.selectedSsid,
            onValueChange = { vm.onSsidChange(it) },
            label = { Text("Nome rete (SSID)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.wifiPassword,
            onValueChange = { vm.onPasswordChange(it) },
            label = { Text("Password WiFi") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = { vm.nextStep() },
            enabled = state.selectedSsid.isNotBlank(),
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Avanti") }
    }
}

@Composable
private fun NetworkRow(net: WifiNetwork, selected: Boolean, onClick: () -> Unit) {
    val signal = when {
        net.rssi > -55 -> "▋▋▋▋"
        net.rssi > -65 -> "▋▋▋░"
        net.rssi > -75 -> "▋▋░░"
        else -> "▋░░░"
    }
    ListItem(
        headlineContent = { Text(net.ssid) },
        supportingContent = { Text("$signal  ${net.rssi} dBm") },
        trailingContent = { if (!net.open) Icon(Icons.Default.Lock, "Protetta", Modifier.size(18.dp)) },
        leadingContent = { Icon(Icons.Default.Wifi, "WiFi") },
        modifier = Modifier.clickable(onClick = onClick),
        colors = if (selected) ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.primaryContainer)
                 else ListItemDefaults.colors(),
    )
}

// ── Step 2: Telegram ──────────────────────────────────────────────────────────

@Composable
private fun Step2Telegram(state: SetupWizardState, vm: SetupWizardViewModel) {
    Column(
        Modifier.fillMaxSize().padding(24.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("Telegram (opzionale)", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text("Ricevi notifiche su Telegram per allarmi temperatura e promemoria manutenzione.")
        OutlinedTextField(
            value = state.telegramToken,
            onValueChange = { vm.onTelegramTokenChange(it) },
            label = { Text("Token Bot Telegram") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.telegramChatId,
            onValueChange = { vm.onTelegramChatIdChange(it) },
            label = { Text("Chat ID") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
            Text(
                "Crea un bot con @BotFather per ottenere il token.\nOttieni il Chat ID usando @userinfobot.",
                Modifier.padding(12.dp),
                style = MaterialTheme.typography.bodySmall,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { vm.nextStep() }, modifier = Modifier.weight(1f)) { Text("Salta") }
            Button(onClick = { vm.nextStep() }, modifier = Modifier.weight(1f)) { Text("Avanti") }
        }
    }
}

// ── Step 3: DuckDNS ──────────────────────────────────────────────────────────

@Composable
private fun Step3DuckDns(state: SetupWizardState, vm: SetupWizardViewModel) {
    val port = state.lanPort.toIntOrNull() ?: DEFAULT_LAN_PORT
    val urlPreview = if (state.duckdnsDomain.isNotBlank()) {
        com.aquarium.controller.data.PrefsRepository.buildUrl(state.duckdnsDomain.trim(), port)
    } else ""

    Column(
        Modifier.fillMaxSize().padding(24.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("DuckDNS", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text("Accedi all'acquario da remoto tramite un indirizzo fisso.")
        OutlinedTextField(
            value = state.duckdnsDomain,
            onValueChange = { vm.onDuckdnsDomainChange(it.lowercase().trim()) },
            label = { Text("Sottodominio (es. mioaqua)") },
            suffix = { Text(".duckdns.org") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.duckdnsToken,
            onValueChange = { vm.onDuckdnsTokenChange(it) },
            label = { Text("Token DuckDNS") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.lanPort,
            onValueChange = { vm.onLanPortChange(it.filter { c -> c.isDigit() }) },
            label = { Text("Porta LAN") },
            supportingText = { Text("Porta del port-forwarding nel router → porta ESP 80") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        if (urlPreview.isNotBlank()) {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                Text("URL: $urlPreview", Modifier.padding(12.dp), style = MaterialTheme.typography.bodySmall)
            }
        }
        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
            Text(
                "Registra un dominio gratuito su duckdns.org.\n" +
                "Configura il port-forwarding del router: porta esterna → 80 dell'ESP.",
                Modifier.padding(12.dp),
                style = MaterialTheme.typography.bodySmall,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = { vm.nextStep() }, modifier = Modifier.weight(1f)) { Text("Salta") }
            Button(
                onClick = { vm.nextStep() },
                enabled = state.duckdnsDomain.isBlank() || (state.duckdnsToken.isNotBlank() && (state.lanPort.toIntOrNull() ?: 0) in 1..65535),
                modifier = Modifier.weight(1f),
            ) { Text("Avanti") }
        }
    }
}

// ── Step 4: Aquarium profile ─────────────────────────────────────────────────

@Composable
private fun Step4Profile(state: SetupWizardState, vm: SetupWizardViewModel) {
    Column(
        Modifier.fillMaxSize().padding(24.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("Tipo di acquario", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
        Text("Seleziona il preset più adatto al tuo acquario.")
        AquariumType.entries.forEach { type ->
            Card(
                onClick = { vm.onAquariumTypeChange(type) },
                colors = CardDefaults.cardColors(
                    containerColor = if (state.aquariumType == type)
                        MaterialTheme.colorScheme.primaryContainer
                    else MaterialTheme.colorScheme.surface,
                ),
                border = if (state.aquariumType == type)
                    CardDefaults.outlinedCardBorder()
                else null,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(type.label, fontWeight = FontWeight.Bold)
                    Text(type.description, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        Button(onClick = { vm.nextStep() }, modifier = Modifier.fillMaxWidth()) { Text("Avanti") }
    }
}

// ── Step 5: Summary & send ───────────────────────────────────────────────────

@Composable
private fun Step5Summary(
    state: SetupWizardState,
    vm: SetupWizardViewModel,
    onSetupComplete: (String) -> Unit,
) {
    Column(
        Modifier.fillMaxSize().padding(24.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text("Riepilogo", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)

        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("WiFi: ${state.selectedSsid}")
                Text("Telegram: ${if (state.telegramToken.isBlank()) "non configurato" else "configurato"}")
                if (state.duckdnsDomain.isNotBlank()) {
                    Text("DuckDNS: ${state.duckdnsDomain}.duckdns.org (porta ${state.lanPort})")
                } else {
                    Text("DuckDNS: non configurato")
                }
                Text("Profilo: ${state.aquariumType.label}")
            }
        }

        if (state.error != null) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
        }

        when {
            state.pollingSuccess -> {
                Text("✅ ESP online! Apertura interfaccia…", color = MaterialTheme.colorScheme.primary)
            }
            state.pollingFailed -> {
                Text(
                    "Timeout: l'ESP non è raggiungibile.\nVerifica il dominio DuckDNS e il port-forwarding del router.",
                    color = MaterialTheme.colorScheme.error,
                )
                Button(onClick = { vm.startPolling(onSetupComplete) }, modifier = Modifier.fillMaxWidth()) {
                    Text("Riprova")
                }
            }
            state.waitingForReconnect -> {
                Text(
                    "Configurazione inviata! L'ESP si sta riavviando…\n\n" +
                    "Riconnetti il telefono alla tua rete WiFi, poi tocca il pulsante.",
                )
                Button(onClick = { vm.startPolling(onSetupComplete) }, modifier = Modifier.fillMaxWidth()) {
                    Text("Mi sono ricollegato al WiFi")
                }
            }
            state.configSent && state.pollingAttempt > 0 -> {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text("Connessione all'acquario… (${state.pollingAttempt}/12)")
                }
            }
            state.isSending -> {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text("Invio configurazione…")
                }
            }
            else -> {
                Button(
                    onClick = { vm.sendProvision(onSetupComplete) },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Configura ESP") }
            }
        }
    }

    // Auto-navigate on polling success
    LaunchedEffect(state.pollingSuccess) {
        if (state.pollingSuccess) {
            val port = state.lanPort.toIntOrNull() ?: DEFAULT_LAN_PORT
            val url = com.aquarium.controller.data.PrefsRepository.buildUrl(state.duckdnsDomain, port)
            onSetupComplete(url)
        }
    }
}
