package com.aquarium.controller.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.aquarium.controller.data.PrefsRepository

/**
 * Screen for users who already have a configured ESP.
 * They enter the DuckDNS domain (without https:// and without .duckdns.org) plus
 * the LAN port, then hit Connect.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConnectScreen(
    onConnect: (url: String) -> Unit,
    onBack: () -> Unit,
) {
    var domain by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("443") }
    var domainError by remember { mutableStateOf(false) }
    var portError by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Connetti al dispositivo") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Indietro")
                    }
                },
            )
        }
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text(
                "Inserisci l'indirizzo DuckDNS e la porta configurata sull'ESP.",
                style = MaterialTheme.typography.bodyMedium,
            )
            OutlinedTextField(
                value = domain,
                onValueChange = { domain = it.lowercase().trim(); domainError = false },
                label = { Text("Dominio DuckDNS") },
                suffix = { Text(".duckdns.org") },
                isError = domainError,
                supportingText = if (domainError) {{ Text("Inserisci il sottodominio DuckDNS") }} else null,
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = port,
                onValueChange = { port = it.filter { c -> c.isDigit() }; portError = false },
                label = { Text("Porta LAN (default 443)") },
                isError = portError,
                supportingText = if (portError) {{ Text("Porta non valida (1–65535)") }} else null,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            // URL preview
            val portInt = port.toIntOrNull()
            if (domain.isNotBlank() && portInt != null) {
                val preview = PrefsRepository.buildUrl(domain, portInt)
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                    Text("URL: $preview", Modifier.padding(12.dp), style = MaterialTheme.typography.bodySmall)
                }
            }

            Button(
                onClick = {
                    val portNum = port.toIntOrNull()
                    domainError = domain.isBlank()
                    portError = portNum == null || portNum !in 1..65535
                    if (!domainError && !portError) {
                        val url = PrefsRepository.buildUrl(domain, portNum!!)
                        onConnect(url)
                    }
                },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("Connetti")
            }
        }
    }
}
