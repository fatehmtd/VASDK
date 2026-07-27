package com.vasdk.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.RestartAlt
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
internal fun Header(
    statusText: String,
    statusActive: Boolean,
    onOpenAnimationTest: () -> Unit,
    onOpenModelInfo: () -> Unit,
    onOpenSettings: () -> Unit,
    onNewConversation: () -> Unit,
    settingsEnabled: Boolean
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(SurfaceColor)
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Avatar(text = "VA", size = 40.dp)

        Column(modifier = Modifier.weight(1f).padding(start = 14.dp)) {
            Text(
                text = "VASDK Assistant",
                color = TextPrimary,
                fontWeight = FontWeight.Bold,
                fontSize = 17.sp
            )
            Text(
                text = statusText,
                color = TextSecondary,
                fontSize = 12.sp,
                modifier = Modifier.padding(top = 1.dp)
            )
        }

        Box(
            modifier = Modifier
                .size(8.dp)
                .clip(CircleShape)
                .background(if (statusActive) Accent else TextSecondary.copy(alpha = 0.4f))
        )

        IconButton(onClick = onOpenAnimationTest, modifier = Modifier.padding(start = 2.dp)) {
            Icon(
                imageVector = Icons.Default.GraphicEq,
                contentDescription = "Test animations",
                tint = Color.White
            )
        }

        IconButton(onClick = onOpenModelInfo, modifier = Modifier.padding(start = 2.dp)) {
            Icon(
                imageVector = Icons.Default.Info,
                contentDescription = "Model info",
                tint = Color.White
            )
        }

        IconButton(onClick = onNewConversation, modifier = Modifier.padding(start = 2.dp)) {
            Icon(
                imageVector = Icons.Default.RestartAlt,
                contentDescription = "New conversation",
                tint = Color.White
            )
        }

        IconButton(
            onClick = onOpenSettings,
            enabled = settingsEnabled,
            modifier = Modifier.padding(start = 2.dp)
        ) {
            Icon(
                imageVector = Icons.Default.Settings,
                contentDescription = "Settings",
                tint = if (settingsEnabled) Color.White else TextSecondary.copy(alpha = 0.4f)
            )
        }
    }
}
