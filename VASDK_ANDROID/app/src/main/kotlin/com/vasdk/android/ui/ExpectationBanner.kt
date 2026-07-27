package com.vasdk.android.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// A slim in-flow banner (not a dialog/overlay) shown while the agent awaits a spoken reply -
// a tool confirmation (make_phone_call/send_sms) or any LLM question flagged isExpecting.
// Never blocks input. `progress` counts down 1f→0f over the expectation window, with the
// bar/dot color burning out from accent to warning as it nears zero.
@Composable
internal fun ExpectationBanner(progress: Float) {
    val animatedProgress by animateFloatAsState(
        targetValue = progress,
        animationSpec = tween(durationMillis = 200, easing = LinearEasing),
        label = "expectation-progress"
    )
    val burnColor by animateColorAsState(
        targetValue = lerpColor(BurnoutColor, Accent, animatedProgress),
        animationSpec = tween(durationMillis = 200, easing = LinearEasing),
        label = "expectation-color"
    )

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(burnColor.copy(alpha = 0.16f))
            .padding(horizontal = 20.dp, vertical = 10.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            BouncingDotsRow(dotColor = burnColor, dotSize = 5.dp)
            Text(
                text = "Waiting for your response...",
                color = burnColor,
                fontSize = 12.sp,
                modifier = Modifier.padding(start = 10.dp)
            )
        }
        LinearProgressIndicator(
            progress = { animatedProgress },
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp)
                .height(3.dp),
            color = burnColor,
            trackColor = SurfaceElevated
        )
    }
}

private fun lerpColor(start: Color, end: Color, fraction: Float): Color {
    val f = fraction.coerceIn(0f, 1f)
    return Color(
        red = start.red + (end.red - start.red) * f,
        green = start.green + (end.green - start.green) * f,
        blue = start.blue + (end.blue - start.blue) * f,
        alpha = 1f
    )
}
