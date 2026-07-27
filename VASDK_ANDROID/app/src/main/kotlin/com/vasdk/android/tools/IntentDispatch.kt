package com.vasdk.android.tools

import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONObject

object IntentDispatch {
    private const val TAG = "IntentDispatch"

    // startActivity() runs async on the main thread after we've already returned "launched"
    // (the call may come from a background/JNI thread and we don't want to block it), so a
    // failure here can never change the JSON result the model already got back. Logging it is
    // the only way it's not a total black hole - a missing permission once threw a
    // SecurityException that was silently swallowed here while the tool still reported success.
    private fun startActivityLogged(context: Context, intent: Intent) {
        try {
            context.startActivity(intent)
        } catch (e: Exception) {
            Log.e(TAG, "startActivity failed for $intent", e)
        }
    }

    // Posts an intent to the main thread and returns immediately with a "launched" confirmation.
    // Safe to call from any thread (including the C++ worker thread).
    fun fire(context: Context, intent: Intent): String {
        val flaggedIntent = intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        // resolveActivity is safe off the main thread, so check up front rather than
        // reporting "launched" for an intent nothing on the device can handle.
        if (flaggedIntent.resolveActivity(context.packageManager) == null) {
            return JSONObject().put("error", "No app on this device can handle this action").toString()
        }
        Handler(Looper.getMainLooper()).post { startActivityLogged(context, flaggedIntent) }
        return JSONObject().put("status", "launched").toString()
    }

    // Same as fire() but returns a richer result with an optional action description.
    fun fire(context: Context, intent: Intent, action: String): String {
        val flaggedIntent = intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (flaggedIntent.resolveActivity(context.packageManager) == null) {
            return JSONObject().put("error", "No app on this device can handle this action").toString()
        }
        Handler(Looper.getMainLooper()).post { startActivityLogged(context, flaggedIntent) }
        return JSONObject().apply {
            put("status", "launched")
            put("action", action)
        }.toString()
    }
}
