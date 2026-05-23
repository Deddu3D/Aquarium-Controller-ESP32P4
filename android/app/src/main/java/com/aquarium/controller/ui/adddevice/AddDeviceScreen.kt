package com.aquarium.controller.ui.adddevice

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavController
import com.aquarium.controller.ui.nav.Screen

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AddDeviceScreen(
    navController: NavController,
    prefillDomain: String = "",
    viewModel: AddDeviceViewModel = hiltViewModel()
) {
    val uiState by viewModel.uiState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }

    LaunchedEffect(prefillDomain) {
        viewModel.preFillDomain(prefillDomain)
    }

    LaunchedEffect(uiState.navigateToHome) {
        if (uiState.navigateToHome) {
            viewModel.clearNavigation()
            navController.navigate(Screen.Home.createRoute(0)) {
                popUpTo(0) { inclusive = true }
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
                title = { Text("Add Device") },
                navigationIcon = {
                    if (navController.previousBackStackEntry != null) {
                        IconButton(onClick = { navController.popBackStack() }) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                        }
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
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Icon(
                imageVector = Icons.Default.Wifi,
                contentDescription = null,
                modifier = Modifier
                    .size(56.dp)
                    .align(Alignment.CenterHorizontally),
                tint = MaterialTheme.colorScheme.primary
            )

            Text(
                text = "Connect to your ESP32 Aquarium Controller via DuckDNS.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth()
            )

            OutlinedTextField(
                value = uiState.deviceName,
                onValueChange = { viewModel.updateDeviceName(it) },
                label = { Text("Device name") },
                placeholder = { Text("My Aquarium") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )

            OutlinedTextField(
                value = uiState.duckDnsDomain,
                onValueChange = { viewModel.updateDomain(it) },
                label = { Text("DuckDNS domain") },
                placeholder = { Text("my-aquarium") },
                supportingText = { Text("https://my-aquarium.duckdns.org/") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )

            if (uiState.testResult != null) {
                val isSuccess = uiState.testResult!!.startsWith("✓")
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = if (isSuccess)
                            MaterialTheme.colorScheme.primaryContainer
                        else
                            MaterialTheme.colorScheme.errorContainer
                    )
                ) {
                    Text(
                        text = uiState.testResult!!,
                        modifier = Modifier.padding(12.dp),
                        color = if (isSuccess)
                            MaterialTheme.colorScheme.onPrimaryContainer
                        else
                            MaterialTheme.colorScheme.onErrorContainer
                    )
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = { viewModel.testConnection() },
                    modifier = Modifier.weight(1f),
                    enabled = !uiState.isTesting && !uiState.isSaving
                ) {
                    if (uiState.isTesting) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(16.dp),
                            strokeWidth = 2.dp
                        )
                        Spacer(Modifier.width(8.dp))
                        Text("Testing...")
                    } else {
                        Text("Test")
                    }
                }
                Button(
                    onClick = { viewModel.saveDevice() },
                    modifier = Modifier.weight(1f),
                    enabled = !uiState.isTesting && !uiState.isSaving &&
                            uiState.duckDnsDomain.isNotBlank()
                ) {
                    if (uiState.isSaving) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(16.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary
                        )
                        Spacer(Modifier.width(8.dp))
                        Text("Saving...")
                    } else {
                        Icon(Icons.Default.Add, contentDescription = null)
                        Spacer(Modifier.width(8.dp))
                        Text("Add Device")
                    }
                }
            }

            HorizontalDivider()

            TextButton(
                onClick = { navController.navigate(Screen.Provision.route) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Set up a new device (WiFi provisioning) →")
            }
        }
    }
}
