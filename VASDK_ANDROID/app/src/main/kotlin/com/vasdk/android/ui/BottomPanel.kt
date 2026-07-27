package com.vasdk.android.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.MainUiState
import com.vasdk.android.WaveformMode
import com.vasdk.android.WaveformSource

// The five-band equalizer bars are retired in favour of the orb for both mic
// and TTS, but kept compilable behind this flag in case we bring them back.
private const val USE_WAVEFORM_BARS = false

@Composable
internal fun BottomPanel(state: MainUiState, onCancelClick: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                color = SurfaceColor,
                shape = RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp)
            )
            .padding(horizontal = 32.dp, vertical = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // The orb covers both sides of the conversation now: blue for the user's
        // mic, red for the agent - pulsating while it thinks, band-driven while
        // it speaks. During agent turns the orb doubles as the stop button.
        AnimatedVisibility(visible = state.waveformMode != WaveformMode.HIDDEN || state.isThinking) {
            val cancellable = state.isSpeaking || state.isThinking
            if (USE_WAVEFORM_BARS && state.waveformSource == WaveformSource.SYNTH) {
                // Agent speech: five-band equalizer bars (kept for reactivation).
                WaveformIndicator(
                    mode = state.waveformMode,
                    targetBars = state.waveformBars,
                    modifier = Modifier.padding(bottom = 20.dp)
                )
            } else {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier.padding(bottom = 20.dp)
                ) {
                    VoiceOrb(
                        level = state.waveformLevel,
                        bars = state.waveformBars,
                        palette = if (state.waveformSource == WaveformSource.MIC && !state.isThinking) {
                            MicOrbPalette
                        } else {
                            SynthOrbPalette
                        },
                        thinking = state.isThinking,
                        onTap = if (cancellable) onCancelClick else null
                    )
                    AnimatedVisibility(visible = cancellable) {
                        Text(
                            text = "Tap the orb to stop",
                            color = TextSecondary,
                            fontSize = 13.sp,
                            modifier = Modifier.padding(top = 6.dp)
                        )
                    }
                }
            }
        }

        AnimatedVisibility(visible = state.typingVisible) {
            BouncingDotsChip(
                dotColor = TextPrimary,
                background = SurfaceElevated,
                modifier = Modifier.padding(bottom = 20.dp)
            )
        }

        Spacer(modifier = Modifier.height(12.dp))
    }
}
