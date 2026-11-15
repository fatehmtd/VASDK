package com.vasdk.android.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.MergeType
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.MainMessage

@Composable
internal fun UserMessageItem(message: MainMessage) {
    // A bubble that just took in the user's follow-up flashes, so the merge is seen as it happens.
    val flash = remember { Animatable(0f) }
    LaunchedEffect(message.mergedAtMs) {
        if (message.mergedAtMs > 0L && System.currentTimeMillis() - message.mergedAtMs < MERGE_FLASH_MS) {
            flash.snapTo(1f)
            flash.animateTo(0f, tween(durationMillis = MERGE_FLASH_MS.toInt()))
        }
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 80.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        horizontalArrangement = Arrangement.End
    ) {
        Column(horizontalAlignment = Alignment.End) {
            Surface(
                color = lerp(UserBubble, MergeFlash, flash.value),
                shape = RoundedCornerShape(22.dp),
                modifier = Modifier.alpha(if (message.isBackchannel) 0.55f else 1f)
            ) {
                Text(
                    text = message.text,
                    color = if (message.isPartial) Color.White.copy(alpha = 0.7f) else Color.White,
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp)
                )
            }
            val caption = listOfNotNull(
                if (message.mergedParts > 1) "merged · ${message.mergedParts} parts" else null,
                if (message.isBackchannel) "not answered" else null,
                if (message.speechDurationMs > 0L) formatSpeechDuration(message.speechDurationMs) else null
            ).joinToString("  ·  ")
            if (caption.isNotEmpty()) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.padding(top = 4.dp)
                ) {
                    if (message.mergedParts > 1) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.MergeType,
                            contentDescription = "merged",
                            tint = Accent,
                            modifier = Modifier.size(13.dp).padding(end = 2.dp)
                        )
                    }
                    Text(
                        text = caption,
                        color = if (message.mergedParts > 1) Accent else TextSecondary.copy(alpha = 0.65f),
                        fontSize = 11.sp
                    )
                }
            }
        }
    }
}

private const val MERGE_FLASH_MS = 1200L

@Composable
internal fun AssistantMessageItem(message: MainMessage) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.Top
    ) {
        LogoAvatar(size = 28.dp)
        Column(modifier = Modifier.padding(start = 10.dp).widthIn(max = 360.dp)) {
            Surface(color = SurfaceElevated, shape = RoundedCornerShape(22.dp)) {
                Text(
                    text = message.text,
                    // Cut off: the user went on, and this answered only the first part of their turn
                    color = if (message.isInterrupted) TextSecondary.copy(alpha = 0.6f) else TextPrimary,
                    fontSize = 15.sp,
                    textDecoration = if (message.isInterrupted) TextDecoration.LineThrough else null,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp)
                )
            }
            if (message.isInterrupted) {
                Text(
                    text = "interrupted · you went on",
                    color = TextSecondary.copy(alpha = 0.65f),
                    fontSize = 11.sp,
                    modifier = Modifier.padding(start = 2.dp, top = 5.dp)
                )
            } else if (message.llmTokens > 0 && message.llmDurationMs > 0L) {
                Text(
                    text = formatLlmStats(message.llmTokens, message.llmDurationMs),
                    color = TextSecondary.copy(alpha = 0.65f),
                    fontSize = 11.sp,
                    modifier = Modifier.padding(start = 2.dp, top = 5.dp)
                )
            }
        }
    }
}

@Composable
internal fun ThinkingMessageItem() {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.Top
    ) {
        LogoAvatar(size = 28.dp)
        Surface(
            modifier = Modifier.padding(start = 10.dp),
            color = SurfaceElevated,
            shape = RoundedCornerShape(22.dp)
        ) {
            BouncingDotsRow(
                dotColor = TextPrimary,
                dotSize = 8.dp,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 18.dp)
            )
        }
    }
}

@Composable
internal fun UserSpeakingItem() {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 80.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        horizontalArrangement = Arrangement.End
    ) {
        Surface(color = UserBubble, shape = RoundedCornerShape(22.dp)) {
            BouncingDotsRow(
                dotColor = Color.White,
                dotSize = 8.dp,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 18.dp)
            )
        }
    }
}

@Composable
internal fun ErrorMessageItem(message: MainMessage) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.Top
    ) {
        Avatar(text = "!", size = 28.dp)
        Column(modifier = Modifier.padding(start = 10.dp).widthIn(max = 360.dp)) {
            Surface(color = ErrorBubble, shape = RoundedCornerShape(22.dp)) {
                Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp)) {
                    Text(
                        text = "Pipeline Error",
                        color = ErrorAccent,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        text = message.text,
                        color = TextPrimary,
                        fontSize = 15.sp,
                        modifier = Modifier.padding(top = 4.dp)
                    )
                }
            }
        }
    }
}

private fun formatSpeechDuration(ms: Long): String {
    val secs = ms / 1000.0
    return "%.1fs".format(secs)
}

private fun formatLlmStats(tokens: Int, durationMs: Long): String {
    val secs = durationMs / 1000.0
    val tps = if (secs > 0.0) tokens / secs else 0.0
    return "%d tok  -  %.1f t/s  -  %.1fs".format(tokens, tps, secs)
}
