package com.vasdk.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.SttProvider
import com.vasdk.android.TtsProvider
import com.vasdk.android.VasdkEngine

@Composable
internal fun SettingsOverlay(
    sttProvider: SttProvider,
    sttStreaming: Boolean,
    ttsProvider: TtsProvider,
    useLocalLlm: Boolean,
    llmModelName: String,
    applyEnabled: Boolean,
    applyInProgress: Boolean,
    onSttProviderChange: (SttProvider) -> Unit,
    onSttStreamingChange: (Boolean) -> Unit,
    onTtsProviderChange: (TtsProvider) -> Unit,
    onUseLocalLlmChange: (Boolean) -> Unit,
    onLlmModelChange: (String) -> Unit,
    onApplyClick: () -> Unit,
    onRunSetupClick: () -> Unit,
    onCloseClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(BgColor)
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(SurfaceColor)
                    .padding(horizontal = 20.dp, vertical = 16.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "Settings",
                    color = TextPrimary,
                    fontWeight = FontWeight.Bold,
                    fontSize = 17.sp,
                    modifier = Modifier.weight(1f)
                )
                IconButton(onClick = onCloseClick) {
                    Icon(imageVector = Icons.Default.Close, contentDescription = "Close", tint = TextPrimary)
                }
            }

            Column(
                modifier = Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .padding(20.dp)
            ) {
                SectionHeader("Speech-to-Text")
                ProviderPicker(
                    options = SttProvider.entries,
                    selected = sttProvider,
                    label = { it.label },
                    onSelect = onSttProviderChange
                )
                Column(modifier = Modifier.padding(top = 10.dp)) {
                    SttStreamingRow(streaming = sttStreaming, onToggle = onSttStreamingChange)
                }

                Column(modifier = Modifier.padding(top = 28.dp)) {
                    SectionHeader("Text-to-Speech")
                    ProviderPicker(
                        options = TtsProvider.entries,
                        selected = ttsProvider,
                        label = { it.label },
                        onSelect = onTtsProviderChange
                    )
                }

                Column(modifier = Modifier.padding(top = 28.dp)) {
                    SectionHeader("Language Model")
                    LlmModeRow(useLocalLlm = useLocalLlm, onToggle = onUseLocalLlmChange)
                    if (useLocalLlm) {
                        Column(modifier = Modifier.padding(top = 10.dp)) {
                            ProviderPicker(
                                options = VasdkEngine.LOCAL_LLM_MODELS,
                                selected = VasdkEngine.LOCAL_LLM_MODELS.firstOrNull { it.modelName == llmModelName },
                                label = { it.modelName },
                                onSelect = { onLlmModelChange(it.modelName) }
                            )
                        }
                    }
                }
            }

            Column(modifier = Modifier.padding(horizontal = 20.dp, vertical = 16.dp)) {
                Button(
                    onClick = onApplyClick,
                    enabled = applyEnabled && !applyInProgress,
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = Accent)
                ) {
                    if (applyInProgress) {
                        CircularProgressIndicator(
                            modifier = Modifier.padding(end = 8.dp).size(16.dp),
                            color = Color.White,
                            strokeWidth = 2.dp
                        )
                    }
                    Text(if (applyInProgress) "Applying..." else "Apply Config")
                }
                Text(
                    text = "Restarts the pipeline with the selected providers.",
                    color = TextSecondary,
                    fontSize = 11.sp,
                    modifier = Modifier.padding(top = 8.dp)
                )
                TextButton(
                    onClick = onRunSetupClick,
                    enabled = !applyInProgress,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Run setup again", color = Accent)
                }
            }
        }
    }
}

@Composable
internal fun SectionHeader(text: String) {
    Text(
        text = text.uppercase(),
        color = Accent,
        fontSize = 12.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = 0.8.sp,
        modifier = Modifier.padding(bottom = 10.dp)
    )
}

@Composable
internal fun <T> ProviderPicker(
    options: List<T>,
    selected: T?,
    label: (T) -> String,
    onSelect: (T) -> Unit,
    caption: (T) -> String? = { null }
) {
    Column {
        options.forEachIndexed { index, option ->
            val isSelected = option == selected
            Surface(
                color = SurfaceElevated,
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = if (index == 0) 0.dp else 8.dp)
                    .clickable { onSelect(option) }
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    RadioButton(
                        selected = isSelected,
                        onClick = { onSelect(option) },
                        colors = RadioButtonDefaults.colors(selectedColor = Accent, unselectedColor = TextSecondary)
                    )
                    Column(modifier = Modifier.padding(start = 4.dp)) {
                        Text(text = label(option), color = TextPrimary, fontSize = 14.sp)
                        caption(option)?.let {
                            Text(text = it, color = BurnoutColor, fontSize = 11.sp)
                        }
                    }
                }
            }
        }
    }
}

@Composable
internal fun SttStreamingRow(streaming: Boolean, onToggle: (Boolean) -> Unit) {
    Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = if (streaming) "Streaming" else "One-shot",
                color = TextPrimary,
                fontSize = 14.sp
            )
            Switch(
                checked = streaming,
                onCheckedChange = onToggle,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = Accent
                )
            )
        }
    }
}

@Composable
internal fun LlmModeRow(useLocalLlm: Boolean, onToggle: (Boolean) -> Unit) {
    Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = if (useLocalLlm) "On-device" else "Cloud",
                color = TextPrimary,
                fontSize = 14.sp
            )
            Switch(
                checked = useLocalLlm,
                onCheckedChange = onToggle,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = Accent
                )
            )
        }
    }
}
