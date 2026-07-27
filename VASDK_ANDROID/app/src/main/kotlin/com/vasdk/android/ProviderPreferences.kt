package com.vasdk.android

import android.content.Context

/**
 * Persists the user's STT/TTS provider and local-LLM-model choices from the Settings UI.
 * Mirrors [LlmPreferences]'s plain-SharedPreferences pattern rather than sharing its file,
 * since these are a distinct set of knobs the Settings screen owns.
 */
object ProviderPreferences {
    private const val PREFS_NAME = "vasdk_provider_prefs"
    private const val KEY_STT_PROVIDER = "stt_provider"
    private const val KEY_STT_STREAMING = "stt_streaming"
    private const val KEY_TTS_PROVIDER = "tts_provider"
    private const val KEY_LOCAL_LLM_MODEL = "local_llm_model_name"

    fun getSttProvider(context: Context): SttProvider {
        val id = prefs(context).getString(KEY_STT_PROVIDER, SttProvider.PARAKEET.id)
        return SttProvider.entries.firstOrNull { it.id == id } ?: SttProvider.PARAKEET
    }

    fun setSttProvider(context: Context, provider: SttProvider) {
        prefs(context).edit().putString(KEY_STT_PROVIDER, provider.id).apply()
    }

    /** Whether the STT provider processes continuous audio (streaming) or discrete segments (one-shot). */
    fun getSttStreaming(context: Context): Boolean =
        prefs(context).getBoolean(KEY_STT_STREAMING, true)

    fun setSttStreaming(context: Context, streaming: Boolean) {
        prefs(context).edit().putBoolean(KEY_STT_STREAMING, streaming).apply()
    }

    fun getTtsProvider(context: Context): TtsProvider {
        val id = prefs(context).getString(KEY_TTS_PROVIDER, TtsProvider.POCKET.id)
        return TtsProvider.entries.firstOrNull { it.id == id } ?: TtsProvider.POCKET
    }

    fun setTtsProvider(context: Context, provider: TtsProvider) {
        prefs(context).edit().putString(KEY_TTS_PROVIDER, provider.id).apply()
    }

    fun getLocalLlmModelName(context: Context): String? =
        prefs(context).getString(KEY_LOCAL_LLM_MODEL, null)

    fun setLocalLlmModelName(context: Context, modelName: String) {
        prefs(context).edit().putString(KEY_LOCAL_LLM_MODEL, modelName).apply()
    }

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
}
