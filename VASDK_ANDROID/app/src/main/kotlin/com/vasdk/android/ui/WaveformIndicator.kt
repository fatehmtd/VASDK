package com.vasdk.android.ui

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.unit.dp
import com.vasdk.android.WaveformMode

@Composable
internal fun WaveformIndicator(
    mode: WaveformMode,
    targetBars: List<Float>,
    modifier: Modifier = Modifier,
    animationMs: Int = 128
) {
    val fallbackTransition = rememberInfiniteTransition(label = "waveform-fallback")
    val fallbackBars = remember {
        listOf(
            WaveBarCfg(380, 0, 0.20f, 0.85f),
            WaveBarCfg(430, 90, 0.30f, 1.00f),
            WaveBarCfg(350, 40, 0.15f, 1.00f),
            WaveBarCfg(460, 120, 0.25f, 0.90f),
            WaveBarCfg(400, 65, 0.18f, 0.80f)
        )
    }

    val bars = targetBars.take(5).let { if (it.size == 5) it else List(5) { 1f } }

    Row(
        modifier = modifier.height(44.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        repeat(5) { index ->
            val fallbackScale by fallbackTransition.animateFloat(
                initialValue = fallbackBars[index].min,
                targetValue = fallbackBars[index].max,
                animationSpec = infiniteRepeatable(
                    animation = tween(
                        durationMillis = fallbackBars[index].durationMs,
                        delayMillis = fallbackBars[index].delayMs,
                        easing = FastOutSlowInEasing
                    ),
                    repeatMode = RepeatMode.Reverse
                ),
                label = "fallback-bar-$index"
            )

            val target = when (mode) {
                WaveformMode.FALLBACK -> fallbackScale
                WaveformMode.DATA_DRIVEN -> bars[index]
                WaveformMode.HIDDEN -> 1f
            }

            val displayScale by animateFloatAsState(
                targetValue = target,
                animationSpec = tween(durationMillis = animationMs),
                label = "display-bar-$index"
            )

            // scale must precede clip/background: a graphics layer only transforms
            // modifiers that come after it in the chain, so with scale last the
            // background renders unscaled and the bars never move.
            Box(
                modifier = Modifier
                    .padding(end = if (index == 4) 0.dp else 4.dp)
                    .size(width = 5.dp, height = 32.dp)
                    .scale(scaleX = 1f, scaleY = displayScale)
                    .clip(RoundedCornerShape(999.dp))
                    .background(Accent)
            )
        }
    }
}

private data class WaveBarCfg(
    val durationMs: Int,
    val delayMs: Int,
    val min: Float,
    val max: Float
)
