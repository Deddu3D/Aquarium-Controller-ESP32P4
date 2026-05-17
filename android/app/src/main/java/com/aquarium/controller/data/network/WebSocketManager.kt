package com.aquarium.controller.data.network

import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import com.aquarium.controller.MainActivity
import com.aquarium.controller.data.model.WsStatus
import com.squareup.moshi.Json
import com.squareup.moshi.JsonClass
import com.squareup.moshi.Moshi
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.min

@JsonClass(generateAdapter = true)
data class WsFrame(
    val type: String,
    @Json(name = "uptime_s") val uptimeS: Long = 0,
    @Json(name = "free_heap") val freeHeap: Long = 0,
    @Json(name = "temp_ok") val tempOk: Boolean = true,
    @Json(name = "temp_c") val tempC: Double = 0.0,
    val phase: Int = 0
)

@Singleton
class WebSocketManager @Inject constructor(
    @ApplicationContext private val context: Context,
    private val okHttpClient: OkHttpClient,
    private val moshi: Moshi
) {
    companion object {
        const val CHANNEL_ID = "temp_alarms"
        private const val NOTIF_ID_HIGH = 1001
        private const val NOTIF_ID_LOW = 1002
        private const val TEMP_HIGH = 28.0
        private const val TEMP_LOW = 22.0
    }

    private val _status = MutableStateFlow(WsStatus())
    val status: StateFlow<WsStatus> = _status.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var webSocket: WebSocket? = null
    private var reconnectJob: Job? = null
    private var currentBaseUrl: String? = null
    private var retryDelay = 1_000L

    private val adapter by lazy { moshi.adapter(WsFrame::class.java) }

    fun connect(baseUrl: String) {
        currentBaseUrl = baseUrl
        retryDelay = 1_000L
        disconnect()
        openSocket(baseUrl)
    }

    fun disconnect() {
        reconnectJob?.cancel()
        webSocket?.close(1000, "User disconnected")
        webSocket = null
        _status.value = WsStatus()
    }

    private fun openSocket(baseUrl: String) {
        val wsUrl = baseUrl
            .replace("https://", "wss://")
            .replace("http://", "ws://")
            .trimEnd('/') + "/ws"

        val request = Request.Builder().url(wsUrl).build()
        webSocket = okHttpClient.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                retryDelay = 1_000L
                _status.value = _status.value.copy(connected = true)
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                try {
                    val frame = adapter.fromJson(text) ?: return
                    if (frame.type == "status") {
                        val newStatus = WsStatus(
                            tempC = frame.tempC,
                            tempOk = frame.tempOk,
                            uptimeS = frame.uptimeS,
                            freeHeap = frame.freeHeap,
                            phase = frame.phase,
                            connected = true
                        )
                        val previous = _status.value
                        _status.value = newStatus
                        checkTempAlarms(previous.tempC, frame.tempC)
                    }
                } catch (_: Exception) {}
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                _status.value = _status.value.copy(connected = false)
                scheduleReconnect()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                _status.value = _status.value.copy(connected = false)
                if (code != 1000) scheduleReconnect()
            }
        })
    }

    private fun scheduleReconnect() {
        reconnectJob?.cancel()
        reconnectJob = scope.launch {
            delay(retryDelay)
            retryDelay = min(retryDelay * 2, 30_000L)
            currentBaseUrl?.let { openSocket(it) }
        }
    }

    private fun checkTempAlarms(previous: Double, current: Double) {
        val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val intent = Intent(context, MainActivity::class.java)
        val pi = PendingIntent.getActivity(context, 0, intent, PendingIntent.FLAG_IMMUTABLE)

        if (current > TEMP_HIGH && previous <= TEMP_HIGH) {
            val notif = NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_dialog_alert)
                .setContentTitle("Temperature Alert")
                .setContentText("Temperature too high: %.1f°C".format(current))
                .setContentIntent(pi)
                .setAutoCancel(true)
                .build()
            nm.notify(NOTIF_ID_HIGH, notif)
        }

        if (current < TEMP_LOW && previous >= TEMP_LOW) {
            val notif = NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_dialog_alert)
                .setContentTitle("Temperature Alert")
                .setContentText("Temperature too low: %.1f°C".format(current))
                .setContentIntent(pi)
                .setAutoCancel(true)
                .build()
            nm.notify(NOTIF_ID_LOW, notif)
        }
    }
}
