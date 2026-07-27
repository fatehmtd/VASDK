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

enum class AnimationWaveformMode {
    HIDDEN,
    FALLBACK,
    DATA_DRIVEN
}

data class AnimationTestUiState(
    val waveformMode: AnimationWaveformMode = AnimationWaveformMode.HIDDEN,
    val waveformBars: List<Float> = List(5) { 1f },
    val rms: Float = 0.5f,
    val bass: Float = 0.2f,
    val lowMid: Float = 0.4f,
    val mid: Float = 0.8f,
    val hiMid: Float = 0.5f,
    val treble: Float = 0.3f,
    val dataDrivenEnabled: Boolean = false
)

class AnimationTestViewModel : ViewModel() {

    private val _uiState = MutableStateFlow(AnimationTestUiState())
    val uiState: StateFlow<AnimationTestUiState> = _uiState.asStateFlow()

    private val synthFeaturesQueue = ArrayDeque<FloatArray>()
    private var featuresTickJob: Job? = null

    fun onStartFallbackClicked() {
        _uiState.update {
            it.copy(
                waveformMode = AnimationWaveformMode.FALLBACK,
                dataDrivenEnabled = false,
                waveformBars = List(5) { 1f }
            )
        }
        stopFeaturesTick()
    }

    fun onStopAnimationClicked() {
        stopFeaturesTick()
        _uiState.update {
            it.copy(
                waveformMode = AnimationWaveformMode.HIDDEN,
                dataDrivenEnabled = false,
                waveformBars = List(5) { 1f }
            )
        }
    }

    fun onToggleDataDrivenClicked() {
        val enabled = !_uiState.value.dataDrivenEnabled
        _uiState.update {
            it.copy(
                dataDrivenEnabled = enabled,
                waveformMode = if (enabled) AnimationWaveformMode.DATA_DRIVEN else it.waveformMode
            )
        }
        if (enabled) {
            startFeaturesTick()
        } else {
            stopFeaturesTick()
        }
    }

    fun onPushMockFeatureClicked() {
        val ui = _uiState.value
        enqueueFeatures(
            floatArrayOf(
                ui.rms,
                ui.bass,
                ui.lowMid,
                ui.mid,
                ui.hiMid,
                ui.treble
            )
        )
        if (!_uiState.value.dataDrivenEnabled) {
            _uiState.update {
                it.copy(
                    dataDrivenEnabled = true,
                    waveformMode = AnimationWaveformMode.DATA_DRIVEN
                )
            }
        }
        startFeaturesTick()
    }

    fun onRmsChanged(value: Float) {
        _uiState.update { it.copy(rms = value) }
    }

    fun onBassChanged(value: Float) {
        _uiState.update { it.copy(bass = value) }
    }

    fun onLowMidChanged(value: Float) {
        _uiState.update { it.copy(lowMid = value) }
    }

    fun onMidChanged(value: Float) {
        _uiState.update { it.copy(mid = value) }
    }

    fun onHiMidChanged(value: Float) {
        _uiState.update { it.copy(hiMid = value) }
    }

    fun onTrebleChanged(value: Float) {
        _uiState.update { it.copy(treble = value) }
    }

    fun onAudioFeatures(source: String, features: FloatArray) {
        if (source != "MIC") return
        val stride = 6
        if (features.size < stride) return

        if (!_uiState.value.dataDrivenEnabled) {
            _uiState.update {
                it.copy(
                    dataDrivenEnabled = true,
                    waveformMode = AnimationWaveformMode.DATA_DRIVEN
                )
            }
        }

        var offset = 0
        while (offset + stride <= features.size) {
            enqueueFeatures(features.copyOfRange(offset, offset + stride))
            offset += stride
        }

        startFeaturesTick()
    }

    private fun enqueueFeatures(features: FloatArray) {
        synthFeaturesQueue.addLast(features)
    }

    private fun startFeaturesTick() {
        if (featuresTickJob?.isActive == true) return

        featuresTickJob = viewModelScope.launch {
            while (isActive && _uiState.value.dataDrivenEnabled) {
                synthFeaturesQueue.removeFirstOrNull()?.let { applyBarsFromFeatures(it) }
                delay(128)
            }
        }
    }

    private fun stopFeaturesTick() {
        featuresTickJob?.cancel()
        featuresTickJob = null
        synthFeaturesQueue.clear()
    }

    private fun applyBarsFromFeatures(features: FloatArray) {
        val rms = features[0]
        val bands = floatArrayOf(features[1], features[2], features[3], features[4], features[5])
        val bars = List(5) { index ->
            val intensity = (bands[index] * 0.8f + rms * 0.2f)
            (intensity * 10.0f).coerceIn(0.15f, 2.5f)
        }
        _uiState.update {
            it.copy(
                waveformMode = AnimationWaveformMode.DATA_DRIVEN,
                waveformBars = bars
            )
        }
    }

    override fun onCleared() {
        stopFeaturesTick()
        super.onCleared()
    }
}
