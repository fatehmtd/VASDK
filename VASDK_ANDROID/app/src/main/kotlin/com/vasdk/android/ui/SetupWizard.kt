package com.vasdk.android.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vasdk.android.BuildConfig
import com.vasdk.android.SetupStep
import com.vasdk.android.SttProvider
import com.vasdk.android.TtsProvider
import com.vasdk.android.VasdkEngine

/**
 * First-run setup: lets the user pick STT/TTS/LLM providers before anything is downloaded,
 * instead of pulling the default on-device models unconditionally. Selections are the same
 * pending settings* values the Settings screen edits; [onFinish] applies them.
 */
@Composable
internal fun SetupWizard(
    step: SetupStep,
    sttProvider: SttProvider,
    sttStreaming: Boolean,
    ttsProvider: TtsProvider,
    useLocalLlm: Boolean,
    llmModelName: String,
    onSttProviderChange: (SttProvider) -> Unit,
    onSttStreamingChange: (Boolean) -> Unit,
    onTtsProviderChange: (TtsProvider) -> Unit,
    onUseLocalLlmChange: (Boolean) -> Unit,
    onLlmModelChange: (String) -> Unit,
    onNext: () -> Unit,
    onBack: () -> Unit,
    onFinish: () -> Unit
) {
    BackHandler(enabled = step != SetupStep.WELCOME, onBack = onBack)

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(BgDark)
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            StepIndicator(step = step)

            Column(
                modifier = Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .padding(20.dp)
            ) {
                when (step) {
                    SetupStep.WELCOME -> WelcomeStep()
                    SetupStep.STT -> {
                        StepTitle("Speech-to-Text", "How your voice is transcribed.")
                        ProviderPicker(
                            options = SttProvider.entries,
                            selected = sttProvider,
                            label = { it.label },
                            onSelect = onSttProviderChange,
                            caption = { if (it.isMissingApiKey()) "No API key configured" else null }
                        )
                        Column(modifier = Modifier.padding(top = 10.dp)) {
                            SttStreamingRow(streaming = sttStreaming, onToggle = onSttStreamingChange)
                        }
                    }
                    SetupStep.TTS -> {
                        StepTitle("Text-to-Speech", "The voice the assistant answers with.")
                        ProviderPicker(
                            options = TtsProvider.entries,
                            selected = ttsProvider,
                            label = { it.label },
                            onSelect = onTtsProviderChange,
                            caption = { if (it.isMissingApiKey()) "No API key configured" else null }
                        )
                    }
                    SetupStep.LLM -> {
                        StepTitle("Language Model", "Run the assistant's brain on-device or in the cloud.")
                        LlmModeRow(useLocalLlm = useLocalLlm, onToggle = onUseLocalLlmChange)
                        if (!useLocalLlm && BuildConfig.LLM_API_KEY.isEmpty()) {
                            Text(
                                text = "No cloud LLM API key configured",
                                color = BurnoutColor,
                                fontSize = 11.sp,
                                modifier = Modifier.padding(top = 8.dp)
                            )
                        }
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
                    SetupStep.REVIEW -> ReviewStep(
                        sttProvider = sttProvider,
                        sttStreaming = sttStreaming,
                        ttsProvider = ttsProvider,
                        useLocalLlm = useLocalLlm,
                        llmModelName = llmModelName
                    )
                }
            }

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp, vertical = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                if (step != SetupStep.WELCOME) {
                    OutlinedButton(onClick = onBack, modifier = Modifier.weight(1f)) {
                        Text("Back", color = TextPrimary)
                    }
                }
                val isLast = step == SetupStep.REVIEW
                Button(
                    onClick = if (isLast) onFinish else onNext,
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = Accent)
                ) {
                    Text(
                        when (step) {
                            SetupStep.WELCOME -> "Get started"
                            SetupStep.REVIEW -> "Download & start"
                            else -> "Next"
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun StepIndicator(step: SetupStep) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(SurfaceColor)
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = "Setup",
            color = TextPrimary,
            fontWeight = FontWeight.Bold,
            fontSize = 17.sp,
            modifier = Modifier.weight(1f)
        )
        SetupStep.entries.forEach { s ->
            Box(
                modifier = Modifier
                    .padding(start = 6.dp)
                    .size(8.dp)
                    .background(
                        color = if (s.ordinal <= step.ordinal) Accent else SurfaceElevated,
                        shape = CircleShape
                    )
            )
        }
    }
}

@Composable
private fun StepTitle(title: String, subtitle: String) {
    Column(modifier = Modifier.padding(bottom = 16.dp)) {
        SectionHeader(title)
        Text(text = subtitle, color = TextSecondary, fontSize = 13.sp)
    }
}

@Composable
private fun WelcomeStep() {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 40.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        LogoAvatar(size = 80.dp)
        Text(
            text = "Welcome to VASDK Assistant",
            color = TextPrimary,
            fontSize = 22.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(top = 20.dp)
        )
        Text(
            text = "Choose how speech is recognised, how the assistant speaks and which language " +
                "model it uses. Only the on-device models you pick will be downloaded.",
            color = TextSecondary,
            fontSize = 14.sp,
            modifier = Modifier.padding(top = 12.dp)
        )
        Text(
            text = "You can change all of this later in Settings.",
            color = TextSecondary.copy(alpha = 0.6f),
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 12.dp)
        )
    }
}

@Composable
private fun ReviewStep(
    sttProvider: SttProvider,
    sttStreaming: Boolean,
    ttsProvider: TtsProvider,
    useLocalLlm: Boolean,
    llmModelName: String
) {
    StepTitle("Review", "These choices are saved and can be changed later in Settings.")
    Surface(color = SurfaceElevated, shape = RoundedCornerShape(12.dp)) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp)) {
            ReviewRow("Speech-to-Text", "${sttProvider.label} · ${if (sttStreaming) "streaming" else "one-shot"}")
            ReviewRow("Text-to-Speech", ttsProvider.label)
            ReviewRow("Language model", if (useLocalLlm) "$llmModelName (on-device)" else "Cloud")
        }
    }
    val downloads = buildList {
        if (sttProvider.isOnDevice()) add(sttProvider.label)
        if (ttsProvider.isOnDevice()) add(ttsProvider.label)
        if (useLocalLlm) add(llmModelName)
        add("Speaker identification model")
    }
    Text(
        text = "Will download: ${downloads.joinToString(", ")}",
        color = TextSecondary,
        fontSize = 12.sp,
        modifier = Modifier.padding(top = 16.dp)
    )
}

@Composable
private fun ReviewRow(label: String, value: String) {
    Column(modifier = Modifier.padding(vertical = 6.dp)) {
        Text(text = label, color = TextSecondary, fontSize = 11.sp)
        Text(text = value, color = TextPrimary, fontSize = 14.sp)
        Box(modifier = Modifier.height(2.dp))
    }
}

private fun SttProvider.isOnDevice(): Boolean =
    this == SttProvider.PARAKEET || this == SttProvider.WHISTLE

private fun TtsProvider.isOnDevice(): Boolean =
    this == TtsProvider.POCKET || this == TtsProvider.PHONON

private fun SttProvider.isMissingApiKey(): Boolean = when (this) {
    SttProvider.SONIOX -> BuildConfig.SONIOX_API_KEY.isEmpty()
    SttProvider.DEEPGRAM -> BuildConfig.DEEPGRAM_API_KEY.isEmpty()
    SttProvider.GLADIA -> BuildConfig.GLADIA_API_KEY.isEmpty()
    SttProvider.CARTESIA -> BuildConfig.CARTESIA_API_KEY.isEmpty()
    SttProvider.FISHAUDIO -> BuildConfig.FISHAUDIO_API_KEY.isEmpty()
    SttProvider.GRADIUM -> BuildConfig.GRADIUM_API_KEY.isEmpty()
    SttProvider.PARAKEET, SttProvider.WHISTLE -> false
}

private fun TtsProvider.isMissingApiKey(): Boolean = when (this) {
    TtsProvider.SONIOX -> BuildConfig.SONIOX_API_KEY.isEmpty()
    TtsProvider.DEEPGRAM -> BuildConfig.DEEPGRAM_API_KEY.isEmpty()
    TtsProvider.CARTESIA -> BuildConfig.CARTESIA_API_KEY.isEmpty()
    TtsProvider.FISHAUDIO -> BuildConfig.FISHAUDIO_API_KEY.isEmpty()
    TtsProvider.GRADIUM -> BuildConfig.GRADIUM_API_KEY.isEmpty()
    TtsProvider.POCKET, TtsProvider.PHONON -> false
}
