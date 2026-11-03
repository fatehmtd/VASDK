package com.vasdk.android

import android.content.Context
import androidx.annotation.Keep
import ai.desertant.voz.Voz
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Holds the Desert Ant Labs Voz recognizer (official SDK, which owns model download and the
 * license telemetry) and exposes it to the native VozTranscriptionProvider over JNI.
 */
@Keep
object VozRecognizer {
    private var voz: Voz? = null

    @Synchronized
    fun open(context: Context, directory: File) {
        if (voz == null) voz = Voz(context.applicationContext, directory.absolutePath)
    }

    /** Fetches the model ahead of the first utterance; no-op once it is on disk. */
    fun download(context: Context, directory: File) {
        open(context, directory)
        val recognizer = synchronized(this) { voz } ?: return
        if (!recognizer.isDownloaded()) runBlocking { recognizer.download() }
    }

    @Synchronized
    fun close() {
        voz?.close()
        voz = null
    }

    /**
     * Called from native code. Returns {"text","words":[{"text","start","end"}]} with times in
     * seconds, or {"error"} on failure.
     */
    @JvmStatic
    @Synchronized
    fun transcribe(samples: FloatArray, sampleRate: Int): String = try {
        val recognizer = voz ?: throw IllegalStateException("Voz is not open")
        val transcription = runBlocking { recognizer.transcribe(samples, sampleRate.toDouble()) }
        JSONObject()
            .put("text", transcription.text)
            .put("words", JSONArray(transcription.words.map {
                JSONObject().put("text", it.text).put("start", it.start).put("end", it.end)
            }))
            .toString()
    } catch (e: Exception) {
        JSONObject().put("error", e.message ?: e.toString()).toString()
    }
}
