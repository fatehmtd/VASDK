package com.vasdk.android.tools

import android.os.Handler
import android.os.Looper

// Lets tool implementations (invoked on the native worker thread) tell the UI layer that
// a spoken confirmation is pending, without blocking the calling thread or showing a dialog.
// MainActivity registers a listener that forwards this to MainViewModel's UI state.
object ConfirmationSignal {
    @Volatile private var listener: ((Boolean) -> Unit)? = null

    fun setListener(listener: (Boolean) -> Unit) {
        this.listener = listener
    }

    fun notifyAwaiting(awaiting: Boolean) {
        Handler(Looper.getMainLooper()).post { listener?.invoke(awaiting) }
    }
}
