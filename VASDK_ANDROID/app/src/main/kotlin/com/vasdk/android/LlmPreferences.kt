package com.vasdk.android

import android.content.Context

object LlmPreferences {
    private const val PREFS_NAME = "vasdk_llm_prefs"
    private const val KEY_USE_LOCAL_LLM = "use_local_llm"

    fun isLocalLlmEnabled(context: Context): Boolean =
        context.applicationContext
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_USE_LOCAL_LLM, false)

    fun setLocalLlmEnabled(context: Context, enabled: Boolean) {
        context.applicationContext
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_USE_LOCAL_LLM, enabled)
            .apply()
    }
}
