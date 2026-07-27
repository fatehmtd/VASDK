package com.vasdk.android.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.MainMessage

@Composable
internal fun UserMessageItem(message: MainMessage) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 80.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        horizontalArrangement = Arrangement.End
    ) {
        Column(horizontalAlignment = Alignment.End) {
            Surface(
                color = UserBubble,
                shape = RoundedCornerShape(22.dp)
            ) {
                Text(
                    text = message.text,
                    color = if (message.isPartial) Color(0xFFA8A0FF) else Color.White,
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp)
                )
            }
            if (message.speechDurationMs > 0L) {
                Text(
                    text = formatSpeechDuration(message.speechDurationMs),
                    color = TextSecondary.copy(alpha = 0.65f),
                    fontSize = 11.sp,
                    modifier = Modifier.padding(top = 4.dp)
                )
            }
        }
    }
}

@Composable
internal fun AssistantMessageItem(message: MainMessage) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 14.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.Top
    ) {
        Avatar(text = "AI", size = 28.dp)
        Column(modifier = Modifier.padding(start = 10.dp).widthIn(max = 360.dp)) {
            Surface(color = SurfaceElevated, shape = RoundedCornerShape(22.dp)) {
                Text(
                    text = message.text,
                    color = TextPrimary,
                    fontSize = 15.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp)
                )
            }
            if (message.llmTokens > 0 && message.llmDurationMs > 0L) {
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
        Avatar(text = "AI", size = 28.dp)
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
