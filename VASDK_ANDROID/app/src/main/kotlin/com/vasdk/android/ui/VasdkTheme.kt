package com.vasdk.android.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val VasdkDarkColors = darkColorScheme(
    primary = Color(0xFF00C9A7),
    onPrimary = Color.Black,
    secondary = Color(0xFF6C63FF),
    background = Color(0xFF0B0D14),
    surface = Color(0xFF141720),
    onSurface = Color(0xFFF2F3FF)
)

@Composable
fun VasdkTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = VasdkDarkColors,
        content = content
    )
}
