package com.aquarium.controller.data.network

import android.util.Log
import com.aquarium.controller.data.model.RemoteStatus
import com.hivemq.client.mqtt.MqttClient
import com.hivemq.client.mqtt.mqtt3.Mqtt3AsyncClient
import com.hivemq.client.mqtt.mqtt3.message.publish.Mqtt3Publish
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.json.JSONException
import org.json.JSONObject
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

private const val TAG = "MqttRemoteManager"

/**
 * Zero-config MQTT remote access manager.
 *
 * Connects to the public HiveMQ broker over TLS (no account required) and
 * communicates with the ESP32 controller via device-specific MQTT topics:
 *
 *   aquarium/{deviceId}/status   – status published by the ESP (subscribed here)
 *   aquarium/{deviceId}/cmd      – commands sent by the app (published here)
 *   aquarium/{deviceId}/response – command responses from the ESP (subscribed here)
 */
@Singleton
class MqttRemoteManager @Inject constructor() {

    companion object {
        private const val BROKER_HOST = "broker.hivemq.com"
        private const val BROKER_PORT = 8883 // TLS
        private const val TOPIC_PREFIX = "aquarium/"
    }

    private var mqttClient: Mqtt3AsyncClient? = null
    private var currentDeviceId: String = ""
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var reconnectJob: Job? = null

    private val _status = MutableStateFlow(RemoteStatus())
    val status: StateFlow<RemoteStatus> = _status.asStateFlow()

    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    // ── Connection management ──────────────────────────────────────────

    /**
     * Connect to the MQTT broker and subscribe to the given device's topics.
     * Safe to call multiple times; disconnects and reconnects if deviceId changes.
     */
    fun connect(deviceId: String) {
        if (deviceId.isBlank()) {
            Log.w(TAG, "connect() called with blank deviceId – skipping")
            return
        }
        if (deviceId == currentDeviceId && mqttClient?.state?.isConnected == true) {
            Log.d(TAG, "Already connected for deviceId=$deviceId")
            return
        }
        disconnect()
        currentDeviceId = deviceId
        openConnection()
    }

    fun disconnect() {
        reconnectJob?.cancel()
        reconnectJob = null
        try {
            mqttClient?.disconnect()
        } catch (_: Exception) {}
        mqttClient = null
        _connected.value = false
        _status.value = RemoteStatus()
        currentDeviceId = ""
    }

    /** Returns true if connected and a deviceId has been configured. */
    fun isConnected(): Boolean = _connected.value

    // ── Command publishing ────────────────────────────────────────────

    /** Tell the ESP to toggle a relay. */
    fun sendRelayToggle(index: Int, on: Boolean) {
        publish("""{"cmd":"relay_toggle","index":$index,"on":$on}""")
    }

    /** Tell the ESP to change LED state/brightness/colour. */
    fun sendSetLed(on: Boolean? = null, brightness: Int? = null,
                   r: Int? = null, g: Int? = null, b: Int? = null) {
        val obj = JSONObject().apply {
            put("cmd", "set_led")
            on?.let         { put("on", it) }
            brightness?.let { put("brightness", it) }
            r?.let          { put("r", it) }
            g?.let          { put("g", it) }
            b?.let          { put("b", it) }
        }
        publish(obj.toString())
    }

    /** Request a full status update from the ESP. */
    fun sendGetStatus() {
        publish("""{"cmd":"get_status"}""")
    }

    /** Start / stop feeding mode on the ESP. */
    fun sendFeedingStart() { publish("""{"cmd":"feeding_start"}""") }
    fun sendFeedingStop()  { publish("""{"cmd":"feeding_stop"}""") }

    // ── Private helpers ───────────────────────────────────────────────

    private fun topicStatus()   = "$TOPIC_PREFIX$currentDeviceId/status"
    private fun topicCmd()      = "$TOPIC_PREFIX$currentDeviceId/cmd"
    private fun topicResponse() = "$TOPIC_PREFIX$currentDeviceId/response"

    private fun openConnection() {
        val clientId = "android-${UUID.randomUUID()}"
        val client = MqttClient.builder()
            .useMqttVersion3()
            .identifier(clientId)
            .serverHost(BROKER_HOST)
            .serverPort(BROKER_PORT)
            .sslWithDefaultConfig()
            .buildAsync()

        mqttClient = client

        client.connectWith()
            .keepAlive(60)
            .send()
            .whenComplete { _, err ->
                if (err != null) {
                    Log.e(TAG, "MQTT connect failed: ${err.message}")
                    _connected.value = false
                    scheduleReconnect()
                } else {
                    Log.i(TAG, "MQTT connected for device $currentDeviceId")
                    _connected.value = true
                    subscribeToTopics(client)
                    // Request a fresh status immediately after connecting
                    sendGetStatus()
                }
            }
    }

    private fun subscribeToTopics(client: Mqtt3AsyncClient) {
        val statusTopic   = topicStatus()
        val responseTopic = topicResponse()

        client.subscribeWith()
            .topicFilter(statusTopic)
            .qos(com.hivemq.client.mqtt.datatypes.MqttQos.AT_MOST_ONCE)
            .callback(::onMessage)
            .send()
            .whenComplete { _, err ->
                if (err != null) Log.e(TAG, "Subscribe to $statusTopic failed: ${err.message}")
                else Log.d(TAG, "Subscribed to $statusTopic")
            }

        client.subscribeWith()
            .topicFilter(responseTopic)
            .qos(com.hivemq.client.mqtt.datatypes.MqttQos.AT_LEAST_ONCE)
            .callback(::onMessage)
            .send()
            .whenComplete { _, err ->
                if (err != null) Log.e(TAG, "Subscribe to $responseTopic failed: ${err.message}")
                else Log.d(TAG, "Subscribed to $responseTopic")
            }
    }

    private fun publish(payload: String) {
        val client = mqttClient
        if (client == null || !_connected.value) {
            Log.w(TAG, "publish() called but not connected")
            return
        }
        val cmdTopic = topicCmd()
        client.publishWith()
            .topic(cmdTopic)
            .payload(payload.toByteArray(Charsets.UTF_8))
            .qos(com.hivemq.client.mqtt.datatypes.MqttQos.AT_LEAST_ONCE)
            .send()
            .whenComplete { _, err ->
                if (err != null) Log.e(TAG, "Publish to $cmdTopic failed: ${err.message}")
            }
    }

    private fun onMessage(publish: Mqtt3Publish) {
        val payload = publish.payloadAsBytes.toString(Charsets.UTF_8)
        try {
            val json = JSONObject(payload)
            val type = json.optString("type")

            if (type == "status") {
                _status.value = RemoteStatus(
                    tempC        = json.optDouble("temp_c", 0.0),
                    tempOk       = json.optBoolean("temp_ok", true),
                    uptimeS      = json.optLong("uptime_s", 0L),
                    freeHeap     = json.optLong("free_heap", 0L),
                    relay0       = json.optBoolean("relay_0", false),
                    relay1       = json.optBoolean("relay_1", false),
                    relay2       = json.optBoolean("relay_2", false),
                    relay3       = json.optBoolean("relay_3", false),
                    ledOn        = json.optBoolean("led_on", false),
                    ledBrightness = json.optInt("led_brightness", 0),
                    feedingActive = json.optBoolean("feeding_active", false),
                    connected    = true
                )
            }
            // Command responses are logged but not used in UI state for now
        } catch (e: JSONException) {
            Log.w(TAG, "Could not parse MQTT payload: $payload")
        }
    }

    private var reconnectDelay = 2_000L

    private fun scheduleReconnect() {
        if (currentDeviceId.isBlank()) return
        reconnectJob?.cancel()
        reconnectJob = scope.launch {
            delay(reconnectDelay)
            reconnectDelay = minOf(reconnectDelay * 2, 60_000L)
            if (currentDeviceId.isNotBlank()) openConnection()
        }
    }
}
