package com.aquarium.controller

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.Build
import com.aquarium.controller.data.network.WebSocketManager
import dagger.hilt.android.HiltAndroidApp

@HiltAndroidApp
class AquariumApp : Application() {

    override fun onCreate() {
        super.onCreate()
        createNotificationChannels()
    }

    private fun createNotificationChannels() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                WebSocketManager.CHANNEL_ID,
                "Temperature Alarms",
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = "Alerts when aquarium temperature goes out of range"
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }
}
