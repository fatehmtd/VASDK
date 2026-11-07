package com.vasdk.android.services

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.vasdk.android.VasdkEngine

/**
 * A user turn as if spoken, sent from adb: testing on an emulator (or a phone) without talking
 * to it. The manifest requires android.permission.DUMP to send it, which adb's shell holds and
 * apps can't get, so only adb reaches it.
 *
 *     adb shell am broadcast -a com.vasdk.android.SAY --es text "what's the capital of France" \
 *         -n com.vasdk.android/.services.TestInputReceiver
 *
 * The reply is spoken, and shows in the log (tag VASDK) as the LLM's or the agent's.
 */
class TestInputReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != "com.vasdk.android.SAY") return
        val text = intent.getStringExtra("text")?.takeIf { it.isNotBlank() } ?: return
        val engine = VasdkEngine.getInstance(context)
        if (!engine.isStarted()) {
            Log.w("VASDK.TestInput", "not started: \"$text\" dropped")
            return
        }
        Log.i("VASDK.TestInput", "as if said: \"$text\"")
        engine.injectText(text)
    }
}
