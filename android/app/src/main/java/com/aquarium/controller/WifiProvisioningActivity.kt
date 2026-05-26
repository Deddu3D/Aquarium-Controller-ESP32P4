package com.aquarium.controller

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import com.aquarium.controller.databinding.ActivityWifiProvisioningBinding
import com.google.android.material.button.MaterialButton
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

/**
 * First-time WiFi provisioning wizard.
 *
 * Guides the user through connecting to the ESP32 AP ("AquariumSetup"),
 * scanning for home networks via the device's JSON API, and sending the
 * chosen credentials via POST /api/provision.
 *
 * Step 0 – Instructions: connect to "AquariumSetup" AP
 * Step 1 – Network list: populated from GET http://192.168.4.1/api/scan
 * Step 2 – Password entry + optional mDNS hostname
 * Step 3 – Done: device is restarting; continue to URL setup
 */
class WifiProvisioningActivity : AppCompatActivity() {

    companion object {
        private const val PORTAL_BASE        = "http://192.168.4.1"
        private const val SCAN_TIMEOUT_MS    = 12_000
        private const val CONNECT_TIMEOUT_MS = 8_000
    }

    private lateinit var binding: ActivityWifiProvisioningBinding
    private var selectedSsid = ""
    private var selectedOpen = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityWifiProvisioningBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setSupportActionBar(binding.toolbar)
        supportActionBar?.title = getString(R.string.prov_title)

        binding.btnStep0Next.setOnClickListener { startScan() }
        binding.tvSkipSetup.setOnClickListener  { navigateToUrlSetup() }
        binding.btnScanRetry.setOnClickListener { startScan() }

        showStep(0)
    }

    // ── Step navigation ────────────────────────────────────────────

    private fun showStep(step: Int) {
        binding.layoutStep0.visibility = if (step == 0) View.VISIBLE else View.GONE
        binding.layoutStep1.visibility = if (step == 1) View.VISIBLE else View.GONE
        binding.layoutStep2.visibility = if (step == 2) View.VISIBLE else View.GONE
        binding.layoutStep3.visibility = if (step == 3) View.VISIBLE else View.GONE
    }

    // ── Step 1: scan ───────────────────────────────────────────────

    private fun startScan() {
        showStep(1)
        binding.progressScan.visibility   = View.VISIBLE
        binding.tvScanError.visibility    = View.GONE
        binding.btnScanRetry.visibility   = View.GONE
        binding.networkContainer.removeAllViews()

        Thread {
            try {
                val conn = (URL("$PORTAL_BASE/api/scan").openConnection() as HttpURLConnection).apply {
                    connectTimeout = CONNECT_TIMEOUT_MS
                    readTimeout    = SCAN_TIMEOUT_MS
                    requestMethod  = "GET"
                }
                val code = conn.responseCode
                val body = if (code == 200) {
                    BufferedReader(InputStreamReader(conn.inputStream)).readText()
                } else null
                conn.disconnect()

                if (body != null) {
                    val arr   = JSONObject(body).getJSONArray("networks")
                    val items = (0 until arr.length())
                        .map { arr.getJSONObject(it) }
                        .filter { it.getString("ssid").isNotBlank() }
                        .map { Pair(it.getString("ssid"), it.getBoolean("open")) }
                    runOnUiThread { populateNetworkList(items) }
                } else {
                    runOnUiThread { showScanError(getString(R.string.prov_scan_failed)) }
                }
            } catch (e: Exception) {
                runOnUiThread { showScanError(getString(R.string.prov_scan_failed)) }
            }
        }.start()
    }

    private fun populateNetworkList(networks: List<Pair<String, Boolean>>) {
        binding.progressScan.visibility = View.GONE
        binding.networkContainer.removeAllViews()

        if (networks.isEmpty()) {
            showScanError(getString(R.string.prov_no_networks))
            return
        }

        for ((ssid, open) in networks) {
            val label = if (open) "📶  $ssid  🔓" else "📶  $ssid"
            val btn = MaterialButton(
                this, null,
                com.google.android.material.R.attr.materialButtonOutlinedStyle
            ).apply {
                text = label
                setTextColor(getColor(R.color.colorOnSurface))
                strokeColor = android.content.res.ColorStateList.valueOf(
                    getColor(R.color.colorPrimary)
                )
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply { bottomMargin = resources.getDimensionPixelSize(R.dimen.btn_spacing) }
                setOnClickListener {
                    selectedSsid = ssid
                    selectedOpen = open
                    setupStep2()
                }
            }
            binding.networkContainer.addView(btn)
        }
        binding.btnScanRetry.visibility = View.VISIBLE
    }

    private fun showScanError(msg: String) {
        binding.progressScan.visibility = View.GONE
        binding.tvScanError.text        = msg
        binding.tvScanError.visibility  = View.VISIBLE
        binding.btnScanRetry.visibility = View.VISIBLE
    }

    // ── Step 2: password entry ─────────────────────────────────────

    private fun setupStep2() {
        showStep(2)
        binding.tvSelectedSsid.text = getString(R.string.prov_network_label, selectedSsid)

        if (selectedOpen) {
            binding.passwordLayout.hint     = getString(R.string.prov_open_network)
            binding.passwordInput.isEnabled = false
            binding.passwordInput.setText("")
        } else {
            binding.passwordLayout.hint     = getString(R.string.prov_password_hint)
            binding.passwordInput.isEnabled = true
        }
        binding.passwordLayout.error = null
        binding.tvProvisionError.visibility = View.GONE

        binding.btnSendProvision.isEnabled = true
        binding.btnBackToScan.isEnabled    = true

        binding.btnSendProvision.setOnClickListener {
            val password = if (selectedOpen) "" else binding.passwordInput.text?.toString()?.trim() ?: ""
            if (!selectedOpen && password.isEmpty()) {
                binding.passwordLayout.error = getString(R.string.prov_password_required)
                return@setOnClickListener
            }
            binding.passwordLayout.error = null
            val mdns = binding.mdnsInput.text?.toString()?.trim() ?: ""
            sendProvisioning(password, mdns)
        }

        binding.btnBackToScan.setOnClickListener { startScan() }
    }

    // ── Step 2 → send provisioning ────────────────────────────────

    private fun sendProvisioning(password: String, mdns: String) {
        binding.progressProvision.visibility = View.VISIBLE
        binding.btnSendProvision.isEnabled   = false
        binding.btnBackToScan.isEnabled      = false
        binding.tvProvisionError.visibility  = View.GONE

        Thread {
            var success = false
            try {
                val conn = (URL("$PORTAL_BASE/api/provision").openConnection() as HttpURLConnection).apply {
                    connectTimeout = CONNECT_TIMEOUT_MS
                    readTimeout    = CONNECT_TIMEOUT_MS
                    requestMethod  = "POST"
                    setRequestProperty("Content-Type", "application/json")
                    doOutput = true
                }
                val body = JSONObject().apply {
                    put("ssid", selectedSsid)
                    put("password", password)
                    if (mdns.isNotEmpty()) put("mdns", mdns)
                }.toString().toByteArray(Charsets.UTF_8)

                conn.outputStream.use { it.write(body) }
                val code     = conn.responseCode
                val response = if (code == 200) {
                    BufferedReader(InputStreamReader(conn.inputStream)).readText()
                } else null
                conn.disconnect()
                success = response?.contains("\"ok\"") == true
            } catch (_: Exception) {
                /* The device restarts ~500 ms after sending "ok", so the
                 * connection can be forcibly closed before we finish reading.
                 * Treat any I/O error here as a probable success because the
                 * credentials are persisted to NVS before the restart fires. */
                success = true
            }

            val finalSuccess = success
            runOnUiThread {
                binding.progressProvision.visibility = View.GONE
                if (finalSuccess) {
                    showStep(3)
                    binding.btnContinue.setOnClickListener { navigateToUrlSetup() }
                } else {
                    binding.btnSendProvision.isEnabled  = true
                    binding.btnBackToScan.isEnabled     = true
                    binding.tvProvisionError.visibility = View.VISIBLE
                    binding.tvProvisionError.text       = getString(R.string.prov_send_failed)
                }
            }
        }.start()
    }

    // ── Navigation ─────────────────────────────────────────────────

    private fun navigateToUrlSetup() {
        startActivity(Intent(this, SetupActivity::class.java))
        finish()
    }
}
