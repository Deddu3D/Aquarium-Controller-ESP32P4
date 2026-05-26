package com.aquarium.controller

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.net.http.SslError
import android.os.Bundle
import android.view.Menu
import android.view.MenuItem
import android.webkit.SslErrorHandler
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout
import com.aquarium.controller.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var prefs: SharedPreferences

    companion object {
        const val PREFS_NAME = "aquarium_prefs"
        const val KEY_URL = "duckdns_url"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

        // First run: go to provisioning wizard
        if (prefs.getString(KEY_URL, null).isNullOrBlank()) {
            startActivity(Intent(this, WifiProvisioningActivity::class.java))
            finish()
            return
        }

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setSupportActionBar(binding.toolbar)
        supportActionBar?.title = getString(R.string.app_name)

        setupWebView()
        setupSwipeRefresh()

        loadUrl()
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        with(binding.webView.settings) {
            javaScriptEnabled = true
            domStorageEnabled = true
            databaseEnabled = true
            loadWithOverviewMode = true
            useWideViewPort = true
            setSupportZoom(true)
            builtInZoomControls = true
            displayZoomControls = false
            mixedContentMode = android.webkit.WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
        }

        binding.webView.webViewClient = object : WebViewClient() {

            override fun onPageStarted(view: WebView, url: String, favicon: Bitmap?) {
                binding.swipeRefresh.isRefreshing = true
            }

            override fun onPageFinished(view: WebView, url: String) {
                binding.swipeRefresh.isRefreshing = false
            }

            override fun onReceivedError(
                view: WebView,
                request: WebResourceRequest,
                error: android.webkit.WebResourceError
            ) {
                if (request.isForMainFrame) {
                    binding.swipeRefresh.isRefreshing = false
                    Toast.makeText(
                        this@MainActivity,
                        getString(R.string.error_connection),
                        Toast.LENGTH_LONG
                    ).show()
                }
            }

            /**
             * Accept the self-signed certificate used by the ESP32 HTTPS server.
             * The user has deliberately configured this device's URL so the risk
             * is accepted consciously (same as a browser "proceed anyway" click).
             */
            @SuppressLint("WebViewClientOnReceivedSslError")
            override fun onReceivedSslError(
                view: WebView,
                handler: SslErrorHandler,
                error: SslError
            ) {
                handler.proceed()
            }
        }
    }

    private fun setupSwipeRefresh() {
        binding.swipeRefresh.setColorSchemeResources(R.color.colorPrimary)
        binding.swipeRefresh.setOnRefreshListener {
            binding.webView.reload()
        }
    }

    private fun loadUrl() {
        val url = prefs.getString(KEY_URL, "")!!.trim()
        binding.webView.loadUrl(url)
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.menu_main, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            R.id.action_refresh -> {
                binding.webView.reload()
                true
            }
            R.id.action_settings -> {
                showChangeUrlDialog()
                true
            }
            R.id.action_provision_wifi -> {
                startActivity(Intent(this, WifiProvisioningActivity::class.java))
                true
            }
            else -> super.onOptionsItemSelected(item)
        }
    }

    private fun showChangeUrlDialog() {
        val intent = Intent(this, SetupActivity::class.java)
        intent.putExtra(SetupActivity.EXTRA_EDIT_MODE, true)
        startActivity(intent)
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (binding.webView.canGoBack()) {
            binding.webView.goBack()
        } else {
            super.onBackPressed()
        }
    }

    override fun onResume() {
        super.onResume()
        // Reload if URL was changed in SetupActivity
        val currentUrl = prefs.getString(KEY_URL, null)
        if (!currentUrl.isNullOrBlank() &&
            currentUrl.trim() != binding.webView.url?.trim()
        ) {
            loadUrl()
        }
    }
}
