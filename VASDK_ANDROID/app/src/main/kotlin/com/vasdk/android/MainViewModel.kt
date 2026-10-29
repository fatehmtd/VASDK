package com.vasdk.android

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.logging.Logger
import kotlin.math.log10
import kotlin.math.max

enum class MainMessageRole {
    USER,
    ASSISTANT,
    THINKING,
    USER_SPEAKING,
    ERROR
}

enum class WaveformMode {
    HIDDEN,
    FALLBACK,
    DATA_DRIVEN
}

/** Which audio stream currently drives the waveform bars. */
enum class WaveformSource {
    NONE,
    MIC,
    SYNTH
}

data class MainMessage(
    val id: Long,
    val role: MainMessageRole,
    val text: String = "",
    val isPartial: Boolean = false,
    val speechDurationMs: Long = 0L,
    val llmTokens: Int = 0,
    val llmDurationMs: Long = 0L
)

data class MainUiState(
    val messages: List<MainMessage> = emptyList(),
    val statusText: String = "Initializing...",
    val isSpeaking: Boolean = false,
    val isThinking: Boolean = false,
    val statusDotActive: Boolean = false,
    val typingVisible: Boolean = false,
    val waveformMode: WaveformMode = WaveformMode.HIDDEN,
    val waveformSource: WaveformSource = WaveformSource.NONE,
    val waveformBars: List<Float> = List(5) { 1f },
    // Overall 0..1 loudness of the stream driving the waveform/orb (dB-mapped).
    val waveformLevel: Float = 0f,
    val downloadOverlayVisible: Boolean = true,
    val initOverlayVisible: Boolean = false,
    val downloadFileName: String = "Verifying...",
    val downloadFilePercent: Int = 0,
    val downloadOverallPercent: Int = 0,
    val canWriteSettings: Boolean = true,
    val hasNotificationAccess: Boolean = true,
    val configApplyInProgress: Boolean = false,
    val awaitingResponse: Boolean = false,
    val awaitingResponseProgress: Float = 1f,
    val modelInfoVisible: Boolean = false,
    val modelInfo: ModelInfo? = null,
    val settingsVisible: Boolean = false,
    val settingsSttProvider: SttProvider = SttProvider.PARAKEET,
    val settingsSttStreaming: Boolean = true,
    val settingsTtsProvider: TtsProvider = TtsProvider.POCKET,
    val settingsUseLocalLlm: Boolean = false,
    val settingsLlmModelName: String = VasdkEngine.LFM2_5Meta.modelName,
    // First-run wizard: edits the same settings* selections as the Settings screen, but
    // nothing is downloaded until the user confirms the last step.
    val setupWizardVisible: Boolean = false,
    val setupStep: SetupStep = SetupStep.WELCOME
)

enum class SetupStep {
    WELCOME,
    STT,
    TTS,
    LLM,
    REVIEW
}

class MainViewModel : ViewModel() {
    companion object {
        val TAG = MainViewModel::javaClass.name

        // How long we wait for the user to reply before an "expecting a response" state
        // (a tool confirmation or a question the LLM asked) burns out on its own.
        private const val EXPECTATION_TIMEOUT_MS = 12_000L
        private const val EXPECTATION_TICK_MS = 200L

        // Audio features arrive as raw linear magnitudes (RMS ~0.005-0.36 during speech,
        // per-band spectrum averages 1e-5..2e-2), so a linear gain either pins the UI at
        // its clamp floor or barely moves it. Map loudness through dB instead: speech RMS
        // spans roughly -55..-12 dBFS, which becomes the 0..1 meter range.
        private const val LEVEL_FLOOR_DB = -55f
        private const val LEVEL_CEIL_DB = -12f

        // Per-frame decay of the running per-band peak used to normalize the waveform
        // bars. Features arrive ~every 80 ms, so 0.98 gives a half-life of ~2.7 s.
        private const val BAND_PEAK_DECAY = 0.98f

        private const val BAR_MIN_SCALE = 0.15f
        private const val BAR_MAX_SCALE = 1.5f
    }

    private val logger = Logger.getLogger(TAG)

    private val _uiState = MutableStateFlow(MainUiState())
    val uiState: StateFlow<MainUiState> = _uiState.asStateFlow()

    private var nextMessageId = 1L
    private var streamingMessageId: Long? = null
    private var partialMessageId: Long? = null
    private var thinkingMessageId: Long? = null
    private var userSpeakingMessageId: Long? = null

    private var hasTransitionedToInit = false
    private var hideWaveformJob: Job? = null
    private var isDataDrivenAnimating = false
    private var expectationJob: Job? = null
    // True from the moment a confirmation/question is requested until the agent has actually
    // finished speaking it (SYNTHESIS_COMPLETED) - the visible countdown only starts then.
    private var pendingExpectation = false

    private var speechStartMs = 0L
    private var speechEndMs = 0L
    private var lastUserMessageId: Long? = null
    private var llmStartMs = 0L
    private val llmResponseText = StringBuilder()

    fun onOpenAnimationTestRequested() {
    }

    /** Seeds the Settings screen's pending selections from the engine's active config. */
    fun setInitialConfig(
        sttProvider: SttProvider,
        sttStreaming: Boolean,
        ttsProvider: TtsProvider,
        useLocalLlm: Boolean,
        llmModelName: String
    ) {
        _uiState.update { state ->
            state.copy(
                settingsSttProvider = sttProvider,
                settingsSttStreaming = sttStreaming,
                settingsTtsProvider = ttsProvider,
                settingsUseLocalLlm = useLocalLlm,
                settingsLlmModelName = llmModelName
            )
        }
    }

    fun onSettingsRequested() {
        _uiState.update { it.copy(settingsVisible = true) }
    }

    fun onSettingsDismissed() {
        _uiState.update { it.copy(settingsVisible = false) }
    }

    fun onSettingsSttProviderChanged(provider: SttProvider) {
        _uiState.update { it.copy(settingsSttProvider = provider) }
    }

    fun onSettingsSttStreamingChanged(streaming: Boolean) {
        _uiState.update { it.copy(settingsSttStreaming = streaming) }
    }

    fun onSettingsTtsProviderChanged(provider: TtsProvider) {
        _uiState.update { it.copy(settingsTtsProvider = provider) }
    }

    fun onSettingsUseLocalLlmChanged(useLocal: Boolean) {
        _uiState.update { it.copy(settingsUseLocalLlm = useLocal) }
    }

    fun onSettingsLlmModelChanged(modelName: String) {
        _uiState.update { it.copy(settingsLlmModelName = modelName) }
    }

    // Shown instead of the download overlay until the user has picked providers, so the
    // first launch doesn't pull models for defaults they may not want. Idempotent so that
    // onStart() re-firing (rotation, returning from background) keeps the current step.
    fun onSetupRequired() {
        _uiState.update { state ->
            if (state.setupWizardVisible) return@update state
            state.copy(
                setupWizardVisible = true,
                setupStep = SetupStep.WELCOME,
                downloadOverlayVisible = false,
                initOverlayVisible = false,
                settingsVisible = false,
                statusText = "Setup required"
            )
        }
    }

    fun onSetupNext() {
        _uiState.update { state ->
            val steps = SetupStep.entries
            state.copy(setupStep = steps[(state.setupStep.ordinal + 1).coerceAtMost(steps.lastIndex)])
        }
    }

    fun onSetupBack() {
        _uiState.update { state ->
            state.copy(setupStep = SetupStep.entries[(state.setupStep.ordinal - 1).coerceAtLeast(0)])
        }
    }

    // The download overlay itself is brought up by onConfigApplyRequested(), which the
    // activity's apply path calls right after this.
    fun onSetupFinished() {
        _uiState.update { it.copy(setupWizardVisible = false) }
    }

    fun onModelInfoRequested(info: ModelInfo) {
        _uiState.update { it.copy(modelInfoVisible = true, modelInfo = info) }
    }

    fun onModelInfoDismissed() {
        _uiState.update { it.copy(modelInfoVisible = false) }
    }

    // Shared by the tool confirm-then-execute flow (ConfirmationSignal, on a native worker
    // thread) and by the LLM's own `isExpecting` signal (onLlmResponse) - both just mean
    // "the agent is waiting on the user to say something back," so they drive the same
    // countdown UI rather than two separate indicators. Requesting it only arms the
    // countdown - it doesn't actually start until SYNTHESIS_COMPLETED confirms the agent has
    // finished speaking (see onAgentStateChanged).
    fun onAwaitingConfirmationChanged(awaiting: Boolean) {
        if (awaiting) pendingExpectation = true else clearExpectation()
    }

    private fun startExpectationTimer() {
        expectationJob?.cancel()
        _uiState.update { it.copy(awaitingResponse = true, awaitingResponseProgress = 1f) }
        val startedAt = System.currentTimeMillis()
        expectationJob = viewModelScope.launch {
            while (isActive) {
                val elapsed = System.currentTimeMillis() - startedAt
                val remaining = (1f - elapsed.toFloat() / EXPECTATION_TIMEOUT_MS).coerceIn(0f, 1f)
                if (remaining <= 0f) {
                    _uiState.update { it.copy(awaitingResponse = false, awaitingResponseProgress = 0f) }
                    break
                }
                _uiState.update { it.copy(awaitingResponseProgress = remaining) }
                delay(EXPECTATION_TICK_MS)
            }
        }
    }

    private fun clearExpectation() {
        pendingExpectation = false
        expectationJob?.cancel()
        expectationJob = null
        _uiState.update { it.copy(awaitingResponse = false, awaitingResponseProgress = 1f) }
    }

    fun onConfigApplyRequested() {
        hasTransitionedToInit = false
        resetConversationState()
        _uiState.update { state ->
            state.copy(
                configApplyInProgress = true,
                downloadOverlayVisible = true,
                initOverlayVisible = false,
                settingsVisible = false,
                messages = emptyList(),
                statusText = "Applying configuration..."
            )
        }
    }

    // Called after VasdkEngine.resetConversation() has already interrupted any in-flight
    // response and cleared the native LLM history - this only clears the local UI state
    // (unlike onConfigApplyRequested, the pipeline itself keeps running).
    fun onNewConversationRequested() {
        resetConversationState()
        _uiState.update {
            it.copy(
                messages = emptyList(),
                isThinking = false,
                typingVisible = false,
                awaitingResponse = false,
                awaitingResponseProgress = 1f
            )
        }
    }

    // Clears every piece of transient conversation tracking state so the fresh pipeline
    // (and its cleared LLM history) starts from a clean slate rather than referencing
    // message IDs that no longer exist after messages is emptied above.
    private fun resetConversationState() {
        streamingMessageId = null
        partialMessageId = null
        thinkingMessageId = null
        userSpeakingMessageId = null
        lastUserMessageId = null
        expectationJob?.cancel()
        expectationJob = null
        pendingExpectation = false
        llmResponseText.clear()
    }

    fun onSpecialPermissionStateRefreshed(canWriteSettings: Boolean, hasNotificationAccess: Boolean) {
        _uiState.update {
            it.copy(
                canWriteSettings = canWriteSettings,
                hasNotificationAccess = hasNotificationAccess
            )
        }
    }

    fun onPermissionRequired() {
        _uiState.update {
            it.copy(
                downloadOverlayVisible = false,
                initOverlayVisible = false,
                statusText = "Open VASDK app to grant microphone access",
                isSpeaking = false
            )
        }
    }

    fun onInitCompleted(success: Boolean) {
        if (success) {
            _uiState.update {
                it.copy(
                    downloadOverlayVisible = false,
                    initOverlayVisible = false,
                    configApplyInProgress = false,
                    statusText = "Waiting for permissions..."
                )
            }
        } else {
            _uiState.update {
                it.copy(
                    downloadOverlayVisible = false,
                    initOverlayVisible = false,
                    configApplyInProgress = false,
                    statusText = "Setup failed - restart the app to retry"
                )
            }
        }
    }

    fun onPermissionDenied() {
        _uiState.update {
            it.copy(
                statusText = "Microphone permission required - grant it in Settings",
                isSpeaking = false,
                statusDotActive = false
            )
        }
    }

    fun onPermissionGrantedAndEngineStarted() {
        _uiState.update {
            it.copy(
                isSpeaking = false,
                statusText = "Listening...",
                statusDotActive = true
            )
        }
    }

    fun onEngineStartFailed() {
        _uiState.update {
            it.copy(
                statusText = "Failed to start",
                isSpeaking = false,
                statusDotActive = false
            )
        }
    }

    fun onTranscriptReceived(text: String, isFinal: Boolean) {
        if (text.isBlank()) {
            if (isFinal) {
                removeUserSpeakingBubble()
                partialMessageId?.let { removeMessage(it) }
            }
            return
        }

        if (!isFinal) {
            if (partialMessageId == null) {
                removeUserSpeakingBubble()
                val id = appendMessage(
                    MainMessage(
                        id = nextId(),
                        role = MainMessageRole.USER,
                        text = text,
                        isPartial = true
                    )
                )
                partialMessageId = id
            } else {
                partialMessageId?.let { id ->
                    mutateMessage(id) { message ->
                        message.copy(text = text)
                    }
                }
            }
            _uiState.update { it.copy(statusText = "Listening...") }
            return
        }

        val userId = if (partialMessageId != null) {
            val id = partialMessageId
            if (id != null) {
                mutateMessage(id) { message ->
                    message.copy(text = text, isPartial = false)
                }
            }
            partialMessageId = null
            id
        } else {
            removeUserSpeakingBubble()
            appendMessage(
                MainMessage(
                    id = nextId(),
                    role = MainMessageRole.USER,
                    text = text,
                    isPartial = false
                )
            )
        }

        lastUserMessageId = userId

        val end = if (speechEndMs > speechStartMs) {
            speechEndMs
        } else {
            System.currentTimeMillis()
        }
        val speechMs = if (speechStartMs > 0L) end - speechStartMs else 0L
        if (speechMs > 0L && userId != null) {
            mutateMessage(userId) { message ->
                message.copy(speechDurationMs = speechMs)
            }
        }

        removeThinkingBubble()
        thinkingMessageId = appendMessage(
            MainMessage(
                id = nextId(),
                role = MainMessageRole.THINKING
            )
        )

        llmStartMs = System.currentTimeMillis()
        llmResponseText.clear()

        // A final transcript can land without a SPEECH_ENDED in between (e.g. a
        // partial promoted to final) - make sure the mic waveform doesn't linger.
        if (_uiState.value.waveformSource == WaveformSource.MIC) {
            hideWaveform()
        }
        _uiState.update {
            it.copy(
                typingVisible = false,
                isThinking = true,
                statusText = "Thinking...",
            )
        }
        // The user just replied to whatever was asked - hide the expectation banner
        // even if they declined or changed the subject instead of confirming.
        clearExpectation()
    }

    private fun onSpeechStartedOrOngoing(isStarting: Boolean) {
        if (isStarting) {
            speechStartMs = System.currentTimeMillis()
            speechEndMs = 0L
            // The mic-driven waveform replaces the old typing dots and level meter as
            // the "we hear you" affordance.
            showWaveform(WaveformSource.MIC)
            if (userSpeakingMessageId == null && partialMessageId == null) {
                userSpeakingMessageId = appendMessage(
                    MainMessage(
                        id = nextId(),
                        role = MainMessageRole.USER_SPEAKING
                    )
                )
            }
        }
        _uiState.update {
            it.copy(
                statusText = "Listening...",
                typingVisible = false
            )
        }
    }

    private fun onSpeechEnded() {
        speechEndMs = System.currentTimeMillis()
        removeUserSpeakingBubble()
        if (_uiState.value.waveformSource == WaveformSource.MIC) {
            hideWaveform()
        }
        _uiState.update {
            it.copy(
                statusText = "Thinking...",
                typingVisible = false
            )
        }
    }

    private fun onAgentStateChanged(agentState: String) {
        val parts = agentState.split(":")
        if (parts.size < 3) return
        val currentState = parts[1]
        val transition = parts[2]
        when (transition) {
            "APP_COMMAND", "WAKE_WORD_DETECTED" -> {
                _uiState.update {
                    it.copy(
                        statusText = "Listening...",
                        statusDotActive = true
                    )
                }
            }
            "SYNTHESIS_STARTED" -> {
                removeUserSpeakingBubble()
                removeThinkingBubble()
                showAgentSpeakingAnimation()
            }
            "SYNTHESIS_COMPLETED" -> {
                hideAgentSpeakingAnimation()
                removeThinkingBubble()
                // Only start the countdown once the agent has actually finished speaking the
                // question/confirmation - not while it's still being requested or spoken.
                if (pendingExpectation) {
                    pendingExpectation = false
                    startExpectationTimer()
                }
                onAgentIdleOrWaitingForSpeech(currentState)
            }
            "BARGE_IN_INTERRUPT", "INTERRUPT_RESOLVED" -> {
                hideAgentSpeakingAnimation()
                removeThinkingBubble()
                onAgentIdleOrWaitingForSpeech(currentState)
            }
        }
    }

    private fun onAgentIdleOrWaitingForSpeech(currentState: String) {
        if (currentState == "IDLE") {
            _uiState.update {
                it.copy(
                    statusText = "Tap to speak",
                    statusDotActive = false
                )
            }
        } else if (streamingMessageId == null) {
            _uiState.update { it.copy(statusText = "Waiting for speech...") }
        }
    }

    fun onStatusChanged(status: String) {
        when {
            status == "SPEECH_STARTING" || status == "SPEECH_ONGOING" -> {
                val isStarting = status == "SPEECH_STARTING"
                onSpeechStartedOrOngoing(isStarting)
            }
            status == "SPEECH_ENDED" -> {
                onSpeechEnded()
            }
            status.startsWith("AGENT_STATE:") -> {
                onAgentStateChanged(status)
            }
        }
    }

    fun onLlmResponse(chunk: String, isFinal: Boolean, isExpecting: Boolean = false, choicesJson: String = "[]") {
        if (chunk.isNotEmpty()) {
            llmResponseText.append(chunk)
        }

        if (streamingMessageId == null && chunk.isNotEmpty()) {
            removeThinkingBubble()
            streamingMessageId = appendMessage(
                MainMessage(
                    id = nextId(),
                    role = MainMessageRole.ASSISTANT,
                    text = chunk
                )
            )
        } else if (chunk.isNotEmpty()) {
            streamingMessageId?.let { id ->
                mutateMessage(id) { message ->
                    message.copy(text = message.text + chunk)
                }
            }
        }

        if (!isFinal) return

        removeThinkingBubble()

        val finishedId = streamingMessageId
        val totalChars = llmResponseText.length
        if (finishedId != null && totalChars > 0) {
            val estimatedTokens = maxOf(1, totalChars / 4)
            val llmMs = System.currentTimeMillis() - llmStartMs
            mutateMessage(finishedId) { message ->
                message.copy(llmTokens = estimatedTokens, llmDurationMs = llmMs)
            }
        }

        removeUserSpeakingBubble()
        showAgentSpeakingAnimation()
        streamingMessageId = null
        _uiState.update { it.copy(statusText = "Waiting for speech...") }

        // The LLM marks its own turn as a question awaiting a reply (isExpecting) - reuse the
        // same countdown UI the tool confirm-then-execute flow uses for that case too. This
        // only arms it; the countdown itself starts once SYNTHESIS_COMPLETED confirms the
        // agent has actually finished speaking it.
        if (isExpecting) pendingExpectation = true else clearExpectation()
    }

    fun onPipelineError(errorCode: Int, errorMessage: String) {
        removeUserSpeakingBubble()
        removeThinkingBubble()
        hideAgentSpeakingAnimation()

        val details = buildString {
            append("Pipeline error")
            append(" (code ")
            append(errorCode)
            append(")")
            if (errorMessage.isNotBlank()) {
                append("\n")
                append(errorMessage)
            }
        }

        appendMessage(
            MainMessage(
                id = nextId(),
                role = MainMessageRole.ERROR,
                text = details
            )
        )

        _uiState.update {
            it.copy(
                statusText = "Error received from pipeline",
                typingVisible = false
            )
        }
    }

    fun onDownloadProgress(fileProgress: Float, overallProgress: Float, fileName: String) {
        val filePct = (fileProgress * 100f).toInt().coerceIn(0, 100)
        val overallPct = (overallProgress * 100f).toInt().coerceIn(0, 100)
        _uiState.update {
            it.copy(
                downloadFilePercent = filePct,
                downloadOverallPercent = overallPct,
                downloadFileName = fileName
            )
        }

        if (overallProgress >= 1.0f && !hasTransitionedToInit) {
            hasTransitionedToInit = true
            _uiState.update {
                it.copy(
                    downloadOverlayVisible = false,
                    initOverlayVisible = true
                )
            }
        }
    }

    fun onAudioFeatures(source: String, features: FloatArray) {
        val stride = 6
        if (features.size < stride) return

        // Only the stream that owns the waveform may drive it - during barge-in both
        // MIC and (draining) SYNTH features can arrive interleaved.
        val expectedOwner = when (source) {
            "MIC" -> WaveformSource.MIC
            "SYNTH" -> WaveformSource.SYNTH
            else -> return
        }
        val ui = _uiState.value
        if (ui.waveformSource != expectedOwner || ui.waveformMode == WaveformMode.HIDDEN) return

        if (!isDataDrivenAnimating) {
            isDataDrivenAnimating = true
            _uiState.update { it.copy(waveformMode = WaveformMode.DATA_DRIVEN) }
        }

        var offset = 0
        while (offset + stride <= features.size) {
            applyBarsFromFeatures(features, offset)
            offset += stride
        }
    }

    private fun showAgentSpeakingAnimation() {
        showWaveform(WaveformSource.SYNTH)
        _uiState.update { it.copy(isSpeaking = true) }
    }

    private fun hideAgentSpeakingAnimation() {
        hideWaveform()
    }

    private fun showWaveform(source: WaveformSource) {
        hideWaveformJob?.cancel()
        hideWaveformJob = null
        isDataDrivenAnimating = false
        // MIC and SYNTH magnitudes differ by an order of magnitude - a peak learned
        // from one stream would flatten (or peg) the other, so relearn on handover.
        if (_uiState.value.waveformSource != source) {
            bandPeaks.fill(1e-5f)
        }
        _uiState.update {
            it.copy(
                waveformMode = WaveformMode.FALLBACK,
                waveformSource = source,
                waveformBars = List(5) { 1f },
                waveformLevel = 0f
            )
        }
    }

    private fun hideWaveform() {
        isDataDrivenAnimating = false
        _uiState.update {
            it.copy(waveformBars = List(5) { 1f }, waveformLevel = 0f, isSpeaking = false)
        }
        hideWaveformJob?.cancel()
        hideWaveformJob = viewModelScope.launch {
            delay(210)
            _uiState.update {
                it.copy(waveformMode = WaveformMode.HIDDEN, waveformSource = WaveformSource.NONE)
            }
        }
    }

    // Running peak per frequency band. Bands differ by orders of magnitude (speech has
    // ~1000x less treble than bass energy), so each bar is normalized against its own
    // recent peak instead of an absolute scale - otherwise only the bass bar ever moves.
    private val bandPeaks = FloatArray(5) { 1e-5f }

    /** Maps a linear amplitude/magnitude to a 0..1 level on a dB scale. */
    private fun amplitudeToLevel(amplitude: Float): Float {
        val db = 20f * log10(max(amplitude, 1e-6f))
        return ((db - LEVEL_FLOOR_DB) / (LEVEL_CEIL_DB - LEVEL_FLOOR_DB)).coerceIn(0f, 1f)
    }

    private fun applyBarsFromFeatures(features: FloatArray, offset: Int) {
        val loudness = amplitudeToLevel(features[offset])
        val bars = List(5) { index ->
            val band = features[offset + 1 + index]
            bandPeaks[index] = max(band, bandPeaks[index] * BAND_PEAK_DECAY)
            val relative = band / bandPeaks[index]
            (BAR_MIN_SCALE + relative * loudness * (BAR_MAX_SCALE - BAR_MIN_SCALE))
                .coerceIn(BAR_MIN_SCALE, BAR_MAX_SCALE)
        }
        _uiState.update { it.copy(waveformBars = bars, waveformLevel = loudness) }
    }

    private fun removeThinkingBubble() {
        val id = thinkingMessageId ?: return
        removeMessage(id)
        thinkingMessageId = null
        _uiState.update { it.copy(isThinking = false) }
    }

    private fun removeUserSpeakingBubble() {
        val id = userSpeakingMessageId ?: return
        removeMessage(id)
        userSpeakingMessageId = null
    }

    private fun appendMessage(message: MainMessage): Long {
        _uiState.update { it.copy(messages = it.messages + message) }
        return message.id
    }

    private fun mutateMessage(id: Long, transform: (MainMessage) -> MainMessage) {
        _uiState.update { state ->
            val updated = state.messages.map { message ->
                if (message.id == id) transform(message) else message
            }
            state.copy(messages = updated)
        }
    }

    private fun removeMessage(id: Long) {
        _uiState.update { state ->
            state.copy(messages = state.messages.filterNot { it.id == id })
        }

        if (streamingMessageId == id) streamingMessageId = null
        if (partialMessageId == id) partialMessageId = null
        if (thinkingMessageId == id) thinkingMessageId = null
        if (userSpeakingMessageId == id) userSpeakingMessageId = null
        if (lastUserMessageId == id) lastUserMessageId = null
    }

    private fun nextId(): Long {
        val id = nextMessageId
        nextMessageId += 1L
        return id
    }
}
