package com.vasdk.android.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val VasdkLightColors = lightColorScheme(
    primary = Accent,
    onPrimary = Color.White,
    secondary = Accent,
    onSecondary = Color.White,
    background = BgColor,
    onBackground = TextPrimary,
    surface = SurfaceColor,
    onSurface = TextPrimary,
    surfaceVariant = SurfaceElevated,
    onSurfaceVariant = TextSecondary,
    outline = Separator,
    error = ErrorAccent
)

@Composable
fun VasdkTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = VasdkLightColors,
        content = content
    )
}
