package com.vasdk.android.ui

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.R

@Composable
internal fun Avatar(text: String, size: Dp, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .size(size)
            .clip(CircleShape)
            .background(UserBubble),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = text,
            color = Color.White,
            fontSize = if (size > 40.dp) 22.sp else if (size > 30.dp) 13.sp else 9.sp,
            fontWeight = FontWeight.Bold
        )
    }
}

/** The VASDK mark in a circular badge - used wherever the app or the assistant is shown. */
@Composable
internal fun LogoAvatar(size: Dp, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .size(size)
            .clip(CircleShape)
            .background(SurfaceElevated),
        contentAlignment = Alignment.Center
    ) {
        Image(
            painter = painterResource(R.drawable.ic_vasdk_logo),
            contentDescription = "VASDK",
            modifier = Modifier.size(size * 0.6f)
        )
    }
}

@Composable
internal fun BouncingDotsChip(
    dotColor: Color,
    background: Color,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier,
        color = background,
        shape = RoundedCornerShape(999.dp)
    ) {
        BouncingDotsRow(
            dotColor = dotColor,
            dotSize = 7.dp,
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 12.dp)
        )
    }
}

@Composable
internal fun BouncingDotsRow(
    dotColor: Color,
    dotSize: Dp,
    modifier: Modifier = Modifier
) {
    val transition = rememberInfiniteTransition(label = "dots")
    val values = listOf(0, 160, 320).mapIndexed { index, delayMs ->
        transition.animateFloat(
            initialValue = 0f,
            targetValue = -9f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 650, delayMillis = delayMs, easing = FastOutSlowInEasing),
                repeatMode = RepeatMode.Reverse
            ),
            label = "dot-$index"
        )
    }

    Row(modifier = modifier, verticalAlignment = Alignment.CenterVertically) {
        values.forEachIndexed { index, anim ->
            Box(
                modifier = Modifier
                    .padding(end = if (index == values.lastIndex) 0.dp else 5.dp)
                    .size(dotSize)
                    .graphicsLayer { translationY = anim.value }
                    .background(dotColor, CircleShape)
            )
        }
    }
}

@Composable
internal fun rememberPulseScale(
    enabled: Boolean,
    min: Float = 1f,
    max: Float = 1.10f,
    durationMs: Int = 1400
): Float {
    if (!enabled) return 1f
    val transition = rememberInfiniteTransition(label = "pulse")
    val scale by transition.animateFloat(
        initialValue = min,
        targetValue = max,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = durationMs, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "pulse-scale"
    )
    return scale
}
