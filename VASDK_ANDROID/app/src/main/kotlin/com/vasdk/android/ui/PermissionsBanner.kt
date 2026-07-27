package com.vasdk.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// WRITE_SETTINGS and Notification Listener access have no runtime-permission
// dialog - Android requires a Settings-screen redirect instead, so this is a
// small persistent banner rather than a popup. Renders nothing once granted.
@Composable
internal fun PermissionsBanner(
    canWriteSettings: Boolean,
    hasNotificationAccess: Boolean,
    onEnableWriteSettingsClick: () -> Unit,
    onEnableNotificationAccessClick: () -> Unit
) {
    if (canWriteSettings && hasNotificationAccess) return

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(SurfaceElevated)
            .padding(horizontal = 20.dp, vertical = 8.dp)
    ) {
        if (!canWriteSettings) {
            PermissionsBannerRow(
                text = "Enable brightness control in Settings",
                onClick = onEnableWriteSettingsClick
            )
        }
        if (!hasNotificationAccess) {
            PermissionsBannerRow(
                text = "Enable media info access in Settings",
                onClick = onEnableNotificationAccessClick
            )
        }
    }
}

@Composable
private fun PermissionsBannerRow(text: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = text,
            color = TextSecondary,
            fontSize = 12.sp,
            modifier = Modifier.weight(1f)
        )
        Button(
            onClick = onClick,
            colors = ButtonDefaults.buttonColors(containerColor = Accent)
        ) {
            Text("Enable", fontSize = 12.sp)
        }
    }
}
