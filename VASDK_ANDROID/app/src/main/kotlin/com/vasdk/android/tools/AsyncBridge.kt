package com.vasdk.android.tools

import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

object AsyncBridge {
    // Runs `block`, which must call future.complete() or future.completeExceptionally()
    // asynchronously, then blocks until the result arrives or the timeout expires.
    // Returns null on timeout or exception - callers should convert null to a JSON error.
    fun <T> blockingCall(timeoutMs: Long = 10_000, block: (CompletableFuture<T>) -> Unit): T? {
        val future = CompletableFuture<T>()
        return try {
            block(future)
            future.get(timeoutMs, TimeUnit.MILLISECONDS)
        } catch (e: TimeoutException) {
            future.cancel(true)
            null
        } catch (e: Exception) {
            null
        }
    }
}
