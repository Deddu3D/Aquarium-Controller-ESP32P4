package com.aquarium.controller.ui

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.webkit.SslErrorHandler
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView

/**
 * Full-screen WebView that loads the ESP web UI via the DuckDNS URL.
 *
 * Self-signed SSL certificates are accepted via onReceivedSslError → handler.proceed().
 * This is intentional: the ESP uses a self-signed cert and there is no way to
 * install it as a trusted CA on the device automatically.
 */
@OptIn(ExperimentalMaterial3Api::class)
@SuppressLint("SetJavaScriptEnabled")
@Composable
fun WebViewScreen(
    url: String,
    onChangeUrl: () -> Unit,
) {
    var webViewRef: WebView? by remember { mutableStateOf(null) }
    var isLoading by remember { mutableStateOf(true) }
    var loadError by remember { mutableStateOf(false) }
    var menuExpanded by remember { mutableStateOf(false) }

    BackHandler(enabled = webViewRef?.canGoBack() == true) {
        webViewRef?.goBack()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Aquarium Controller") },
                actions = {
                    IconButton(onClick = { webViewRef?.reload() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Ricarica")
                    }
                    IconButton(onClick = { menuExpanded = true }) {
                        Icon(Icons.Default.MoreVert, contentDescription = "Menu")
                    }
                    DropdownMenu(expanded = menuExpanded, onDismissRequest = { menuExpanded = false }) {
                        DropdownMenuItem(
                            text = { Text("Cambia indirizzo") },
                            onClick = { menuExpanded = false; onChangeUrl() },
                        )
                    }
                },
            )
        }
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            if (loadError) {
                Column(Modifier.fillMaxSize(), verticalArrangement = androidx.compose.foundation.layout.Arrangement.Center) {
                    Text(
                        "Impossibile connettersi all'acquario.\n" +
                        "Verifica la connessione internet e che il dominio DuckDNS sia raggiungibile.",
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = androidx.compose.ui.Modifier.padding(24.dp),
                    )
                    Button(
                        onClick = { loadError = false; webViewRef?.reload() },
                        modifier = androidx.compose.ui.Modifier
                            .padding(horizontal = 24.dp)
                            .fillMaxWidth(),
                    ) { Text("Ricarica") }
                }
            } else {
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { ctx ->
                        WebView(ctx).apply {
                            settings.javaScriptEnabled = true
                            settings.domStorageEnabled = true
                            settings.loadWithOverviewMode = true
                            settings.useWideViewPort = true
                            webViewClient = object : WebViewClient() {
                                override fun onPageStarted(view: WebView, url: String, favicon: Bitmap?) {
                                    isLoading = true
                                    loadError = false
                                }
                                override fun onPageFinished(view: WebView, url: String) {
                                    isLoading = false
                                }
                                override fun onReceivedError(
                                    view: WebView,
                                    errorCode: Int,
                                    description: String,
                                    failingUrl: String,
                                ) {
                                    isLoading = false
                                    loadError = true
                                }
                                @SuppressLint("WebViewClientOnReceivedSslError")
                                override fun onReceivedSslError(
                                    view: WebView,
                                    handler: SslErrorHandler,
                                    error: android.net.http.SslError,
                                ) {
                                    // Accept self-signed certificate from the ESP
                                    handler.proceed()
                                }
                            }
                            webViewRef = this
                            loadUrl(url)
                        }
                    },
                )
                if (isLoading) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
            }
        }
    }
}
