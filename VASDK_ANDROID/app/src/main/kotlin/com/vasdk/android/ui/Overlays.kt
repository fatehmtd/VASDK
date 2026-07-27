package com.vasdk.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.scale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
internal fun DownloadOverlay(fileName: String, filePercent: Int, overallPercent: Int) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(BgDark)
    ) {
        Column(
            modifier = Modifier
                .align(Alignment.Center)
                .fillMaxWidth()
                .padding(horizontal = 40.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Avatar(text = "VA", size = 80.dp)
            Text(
                text = "VASDK Assistant",
                color = TextPrimary,
                fontSize = 24.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(top = 20.dp)
            )
            Text(
                text = "Downloading AI Models",
                color = TextSecondary,
                fontSize = 14.sp,
                modifier = Modifier.padding(top = 6.dp)
            )

            DownloadProgressRow(
                label = fileName,
                percent = filePercent,
                modifier = Modifier.padding(top = 36.dp)
            )
            DownloadProgressRow(
                label = "Overall progress",
                percent = overallPercent,
                modifier = Modifier.padding(top = 20.dp)
            )

            Text(
                text = "This may take a few minutes on first launch",
                color = TextSecondary.copy(alpha = 0.5f),
                fontSize = 11.sp,
                modifier = Modifier.padding(top = 44.dp)
            )
        }
    }
}

@Composable
private fun DownloadProgressRow(label: String, percent: Int, modifier: Modifier = Modifier) {
    Column(modifier = modifier.fillMaxWidth()) {
        Text(
            text = label,
            color = Accent,
            fontSize = 13.sp,
            maxLines = 1
        )
        LinearProgressIndicator(
            progress = { percent / 100f },
            modifier = Modifier
                .fillMaxWidth()
                .height(10.dp)
                .padding(top = 12.dp),
            color = Accent,
            trackColor = SurfaceElevated
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
            horizontalArrangement = Arrangement.End
        ) {
            Text(text = "$percent%", color = TextSecondary, fontSize = 13.sp)
        }
    }
}

@Composable
internal fun InitOverlay() {
    val logoScale = rememberPulseScale(enabled = true, min = 1f, max = 1.14f, durationMs = 1600)

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(BgDark)
    ) {
        Column(
            modifier = Modifier.align(Alignment.Center),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Avatar(text = "VA", size = 80.dp, modifier = Modifier.scale(logoScale))
            Text(
                text = "Preparing your assistant...",
                color = TextPrimary,
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(top = 32.dp)
            )
            Text(
                text = "Loading voice models",
                color = TextSecondary,
                fontSize = 14.sp,
                modifier = Modifier.padding(top = 6.dp)
            )
            BouncingDotsChip(
                dotColor = TextPrimary,
                background = SurfaceElevated,
                modifier = Modifier.padding(top = 28.dp)
            )
        }
    }
}
