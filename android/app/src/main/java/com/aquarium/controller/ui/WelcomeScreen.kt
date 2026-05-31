package com.aquarium.controller.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * First screen shown on fresh install.
 * Lets the user choose between first-time setup (wizard) or connecting to an
 * already-configured ESP via DuckDNS URL.
 */
@Composable
fun WelcomeScreen(
    onFirstSetup: () -> Unit,
    onAlreadyConfigured: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            text = "🐠",
            fontSize = 72.sp,
        )
        Spacer(Modifier.height(16.dp))
        Text(
            text = "Aquarium Controller",
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(8.dp))
        Text(
            text = "Gestisci il tuo acquario da smartphone",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(48.dp))
        Button(
            onClick = onFirstSetup,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Prima configurazione")
        }
        Spacer(Modifier.height(12.dp))
        OutlinedButton(
            onClick = onAlreadyConfigured,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Ho già un ESP configurato")
        }
    }
}
