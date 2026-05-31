package com.aquarium.controller.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// Aquarium-inspired dark ocean color palette
private val Primary = Color(0xFF29B6F6)        // light blue
private val PrimaryContainer = Color(0xFF0D47A1) // deep blue
private val Secondary = Color(0xFF26C6DA)       // cyan
private val Background = Color(0xFF0A1929)      // very dark navy
private val Surface = Color(0xFF0D2137)         // dark blue surface
private val SurfaceVariant = Color(0xFF1A3450)  // slightly lighter surface
private val OnPrimary = Color(0xFF003049)
private val OnBackground = Color(0xFFE0F2F1)
private val OnSurface = Color(0xFFB3E5FC)
private val Error = Color(0xFFEF5350)

private val AquariumDarkColors = darkColorScheme(
    primary = Primary,
    onPrimary = OnPrimary,
    primaryContainer = PrimaryContainer,
    onPrimaryContainer = Color(0xFFBBDEFB),
    secondary = Secondary,
    onSecondary = Color(0xFF003A40),
    secondaryContainer = Color(0xFF004F58),
    onSecondaryContainer = Color(0xFFB2EBF2),
    background = Background,
    onBackground = OnBackground,
    surface = Surface,
    onSurface = OnSurface,
    surfaceVariant = SurfaceVariant,
    onSurfaceVariant = Color(0xFF90CAF9),
    error = Error,
    onError = Color(0xFF601410),
)

@Composable
fun AquariumTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = AquariumDarkColors,
        content = content,
    )
}
