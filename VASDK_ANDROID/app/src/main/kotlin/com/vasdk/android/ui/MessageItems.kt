package com.vasdk.android.ui

import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.font.FontWeight
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
            // Resizes smoothly when a retracted reply gives way to the answer that replaces it
            Surface(
                color = SurfaceElevated,
                shape = RoundedCornerShape(22.dp),
                modifier = Modifier.animateContentSize(tween(durationMillis = 250))
            ) {
                Crossfade(targetState = message.isRetracted, animationSpec = tween(durationMillis = 250), label = "retraction") { retracted ->
                    if (retracted) {
                        RedactedText(message.text)
                    } else {
                        Text(
                            text = message.text,
                            color = TextPrimary,
                            fontSize = 15.sp,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 11.dp)
                        )
                    }
                }
            }
            if (!message.isRetracted && message.llmTokens > 0 && message.llmDurationMs > 0L) {
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

/**
 * A reply the user's follow-up made void: bars sweep over its words, word by word (REDACTION_SWEEP_MS),
 * as the text fades under them; then they pulse while the answer that replaces it is on its way.
 */
@Composable
private fun RedactedText(text: String) {
    val sweep = remember { Animatable(0f) }
    LaunchedEffect(Unit) { sweep.animateTo(1f, tween(durationMillis = REDACTION_SWEEP_MS, easing = FastOutSlowInEasing)) }
    val pulse by rememberInfiniteTransition(label = "redacted").animateFloat(
        initialValue = 0.45f,
        targetValue = 0.8f,
        animationSpec = infiniteRepeatable(tween(durationMillis = 700), RepeatMode.Reverse),
        label = "redactedPulse"
    )
    var layout by remember { mutableStateOf<TextLayoutResult?>(null) }
    val words = remember(text) { Regex("\\S+").findAll(text).map { it.range }.toList() }
    Text(
        text = text,
        color = TextPrimary.copy(alpha = 1f - sweep.value),
        fontSize = 15.sp,
        onTextLayout = { layout = it },
        modifier = Modifier
            .padding(horizontal = 16.dp, vertical = 11.dp)
            .drawWithContent {
                drawContent()
                val laidOut = layout ?: return@drawWithContent
                if (words.isEmpty()) return@drawWithContent
                // Words covered so far; the one being covered grows from its left edge
                val covered = sweep.value * words.size
                val barAlpha = if (sweep.value < 1f) 0.8f else pulse
                for ((index, range) in words.withIndex()) {
                    val fraction = (covered - index).coerceIn(0f, 1f)
                    if (fraction <= 0f) break
                    val first = laidOut.getBoundingBox(range.first)
                    val last = laidOut.getBoundingBox(range.last)
                    // A word laid out over two lines: cover the part on its first line
                    val right = if (last.top == first.top) last.right else laidOut.getLineRight(laidOut.getLineForOffset(range.first))
                    val height = first.bottom - first.top
                    val inset = height * 0.15f
                    drawRoundRect(
                        color = RedactionBar.copy(alpha = barAlpha),
                        topLeft = Offset(first.left, first.top + inset),
                        size = Size((right - first.left) * fraction, height - 2 * inset),
                        cornerRadius = CornerRadius(height / 2f)
                    )
                }
            }
    )
}

private const val REDACTION_SWEEP_MS = 450

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
