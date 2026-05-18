package com.aquarium.controller.ui.connect

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.NetworkWifi
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.ui.nav.Screen

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConnectScreen(
    navController: NavController,
    viewModel: ConnectViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }

    // Auto-redirect to provisioning wizard when no host is saved
    LaunchedEffect(uiState.navigateToProvision) {
        if (uiState.navigateToProvision) {
            viewModel.clearNavigation()
            navController.navigate(Screen.Provision.route) {
                popUpTo(Screen.Connect.route) { inclusive = false }
            }
        }
    }

    LaunchedEffect(uiState.navigateToLogin) {
        if (uiState.navigateToLogin) {
            viewModel.clearNavigation()
            navController.navigate(Screen.Login.route) {
                popUpTo(Screen.Connect.route) { inclusive = false }
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
                title = { Text("Aquarium Controller") },
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
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Icon(
                imageVector = Icons.Default.NetworkWifi,
                contentDescription = null,
                modifier = Modifier
                    .size(64.dp)
                    .align(Alignment.CenterHorizontally),
                tint = MaterialTheme.colorScheme.primary
            )
            Text(
                text = "Connect to Controller",
                style = MaterialTheme.typography.headlineMedium,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )

            OutlinedTextField(
                value = uiState.host,
                onValueChange = { viewModel.updateHost(it) },
                label = { Text("Hostname or IP") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                placeholder = { Text("192.168.1.100 or aquarium.local") }
            )

            OutlinedTextField(
                value = uiState.port,
                onValueChange = { viewModel.updatePort(it) },
                label = { Text("Port") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Use HTTPS", style = MaterialTheme.typography.bodyLarge)
                Switch(
                    checked = uiState.useHttps,
                    onCheckedChange = { viewModel.updateUseHttps(it) }
                )
            }

            OutlinedTextField(
                value = uiState.username,
                onValueChange = { viewModel.updateUsername(it) },
                label = { Text("Username") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = { viewModel.scanMdns() },
                    modifier = Modifier.weight(1f),
                    enabled = !uiState.isScanning
                ) {
                    if (uiState.isScanning) {
                        CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                        Spacer(Modifier.width(8.dp))
                        Text("Scanning...")
                    } else {
                        Icon(Icons.Default.Search, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text("Scan mDNS")
                    }
                }
                Button(
                    onClick = { viewModel.connect() },
                    modifier = Modifier.weight(1f)
                ) {
                    Text("Connect")
                }
            }

            HorizontalDivider()

            TextButton(
                onClick = { navController.navigate(Screen.Provision.route) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Prima configurazione / Nuovo dispositivo →")
            }

            if (uiState.discoveredHosts.isNotEmpty()) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
                ) {
                    Column(modifier = Modifier.padding(8.dp)) {
                        Text(
                            "Discovered devices:",
                            style = MaterialTheme.typography.labelLarge,
                            modifier = Modifier.padding(bottom = 4.dp)
                        )
                        uiState.discoveredHosts.forEach { host ->
                            ListItem(
                                headlineContent = { Text(host) },
                                modifier = Modifier.clickable { viewModel.selectDiscoveredHost(host) }
                            )
                        }
                    }
                }
            }
        }
    }
}
