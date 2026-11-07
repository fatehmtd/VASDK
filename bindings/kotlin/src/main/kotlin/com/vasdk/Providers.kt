// The providers an engine config names by id: those built into the vasdk_providers library, and
// those written in Kotlin (VASDK/include/VASDK/c/vasdk.h: Providers written in the app).
package com.vasdk

import com.sun.jna.Pointer
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.Closeable
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

/**
 * How a provider written in Kotlin gives back what it finds or makes, from any thread, for as long
 * as the provider lives: transcripts (STT), speech (TTS), errors.
 */
class Sink internal constructor(private val handle: Pointer) {
    /** A transcript: final, or a partial. */
    fun text(text: String, final: Boolean = true) = Lib.vasdk.vasdk_sink_text(handle, text, if (final) 1 else 0)

    /** Speech, 16-bit mono at the session's output rate, in order. */
    fun audio(pcm: ShortArray) {
        if (pcm.isNotEmpty()) Lib.vasdk.vasdk_sink_audio(handle, pcm, SizeT(pcm.size.toLong()))
    }

    /** The text given to synthesize() is all said. */
    fun done() = Lib.vasdk.vasdk_sink_done(handle)

    fun error(message: String) = Lib.vasdk.vasdk_sink_error(handle, message)
}

// What each kind of provider implements. close() ends an instance (default: nothing to do).

/** Voice activity: the chance, 0 to 1, that the audio is speech; 0.5 and above counts. */
interface VadProvider : Closeable {
    fun detect(pcm: ShortArray): Float
    override fun close() {}
}

/**
 * Speech to text, results through the Sink. With the config's streaming_transcription off (the
 * default), write() gets a whole utterance once the user stops, then endOfSpeech(): give the final
 * before it returns. With it on, write() gets the audio as they speak; results come any time.
 */
interface SttProvider : Closeable {
    fun write(pcm: ShortArray)
    fun endOfSpeech() {}
    fun start() {}
    fun stop() {}
    override fun close() {}
}

/** Text to speech: start saying `text` (queue it behind the last) and return true; the speech goes to Sink.audio(), then Sink.done(). */
interface TtsProvider : Closeable {
    fun synthesize(text: String): Boolean
    /** Drop what's being said and queued, at a barge-in. */
    fun cancel() {}
    override fun close() {}
}

/** End of turn: the user's audio since they began speaking; the chance, 0 to 1, that they're done. */
interface TurnProvider : Closeable {
    fun evaluate(pcm: ShortArray): Float
    override fun close() {}
}

/** Wake word: each frame of the microphone, in order; the phrase and its confidence when this audio completed it. */
interface WakeWordProvider : Closeable {
    fun process(pcm: ShortArray): Pair<String, Float>?
    override fun close() {}
}

/** Audio DSP: each frame of the microphone's audio, in order, before anything hears it; the
 *  processed samples (as many), or null to pass it on. */
interface AudioDspProvider : Closeable {
    fun process(pcm: ShortArray): ShortArray?
    override fun close() {}
}

/** Speaker embedding: one utterance's voice print, the same length each time; null when it can't be made. */
interface SpeakerEmbeddingProvider : Closeable {
    fun embed(pcm: ShortArray): FloatArray?
    override fun close() {}
}

/** A sentence's vector, normalized to length 1; null when it can't be encoded. One instance for every session, called from several threads. */
interface SentenceEncoderProvider : Closeable {
    fun encode(sentence: String): FloatArray?
    override fun close() {}
}

/** An agent's list entity, as an entity model is told about it. */
data class EntityLabel(val entity: String, val label: String, val description: String, val examples: List<String>)

/** Words marked as an entity: start and length index the words. */
data class EntitySpan(val entity: String, val start: Int, val length: Int, val score: Float = 1f)

/** Where entities are said in the words; `sensitive`: they may hold a secret, keep them on the device. One instance, several threads. */
interface EntityModelProvider : Closeable {
    fun extract(words: List<String>, labels: List<EntityLabel>, sensitive: Boolean): List<EntitySpan>
    override fun close() {}
}

/** One message of an LLM request: role "system", "user", "assistant" or "tool". */
data class LlmMessage(val role: String, val content: String)

/**
 * An LLM: the reply's text to the messages; throwing fails the request. It answers the conversation
 * (the system message holds the instructions and the tools, in the session's chat format) and the
 * agent's one-shot asks. It may block.
 */
interface LlmProvider : Closeable {
    fun complete(messages: List<LlmMessage>): String
    /** Give up the request in flight (from another thread), at a barge-in. */
    fun cancel() {}
    override fun close() {}
}

/**
 * The providers an engine config can name by id. builtIn: every provider built into the
 * vasdk_providers library (addAll()). Those written in Kotlin are added by kind, with a factory
 * making each session's instance from its Sink and sample rate:
 *
 *     val providers = Providers()
 *     providers.vad("loudness") { _, _ -> object : VadProvider { override fun detect(pcm: ShortArray) = ... } }
 *
 * and an engine config names them: providers { vad { id: "loudness" } }. Methods run on VASDK's
 * threads; an exception is logged and counts as no result. Keep it open as long as sessions use it.
 */
class Providers(builtIn: Boolean = true) : Closeable {
    internal var handle: Pointer? = Lib.vasdk.vasdk_providers_create()
    private val callbacks = mutableListOf<Any>()
    private val instances = ConcurrentHashMap<Long, Closeable>()   // id -> the Kotlin object, while C holds it
    private val next = AtomicLong()
    private val live: Pointer get() = handle ?: throw VasdkException("providers closed")

    init {
        if (builtIn) addAll()
    }

    /** Adds every provider built into the vasdk_providers library. */
    fun addAll() = Lib.providers.vasdk_providers_add_all(live)

    private fun <C : com.sun.jna.Callback> keep(callback: C): C = Lib.attached(callback).also { synchronized(callbacks) { callbacks += it } }

    @Suppress("UNCHECKED_CAST")
    private fun <P> of(instance: Pointer?): P? = instance?.let { instances[Pointer.nativeValue(it)] as P? }

    private fun <P : Closeable> lifecycle(id: String, make: (Sink, Int) -> P): Pair<CreateFn, InstanceFn> {
        val create = keep(CreateFn { sink, sampleRate, _ ->
            val made = guarded<P?>("provider $id", null) { make(Sink(sink), sampleRate) } ?: return@CreateFn null
            val key = next.incrementAndGet()
            instances[key] = made
            Pointer(key)
        })
        val destroy = keep(InstanceFn { instance ->
            instance?.let { instances.remove(Pointer.nativeValue(it)) }?.let { guarded("provider $id close") { it.close() } }
        })
        return create to destroy
    }

    private fun added(ok: Int, id: String) {
        if (ok == 0) throw VasdkException("provider $id not added")
    }

    fun vad(id: String, make: (sink: Sink, sampleRate: Int) -> VadProvider) {
        val (create, destroy) = lifecycle(id, make)
        val detect = keep(VadFn { instance, pcm, samples ->
            guarded("vad $id", 0f) { of<VadProvider>(instance)?.detect(pcmAt(pcm, samples)) ?: 0f }
        })
        added(Lib.vasdk.vasdk_providers_add_vad(live, id, create, destroy, detect, null), id)
    }

    fun stt(id: String, make: (sink: Sink, sampleRate: Int) -> SttProvider) {
        val (create, destroy) = lifecycle(id, make)
        fun event(what: String, call: (SttProvider) -> Unit) =
            keep(InstanceFn { instance -> guarded("stt $id $what") { of<SttProvider>(instance)?.let(call) } })
        val write = keep(SttWriteFn { instance, pcm, samples -> guarded("stt $id") { of<SttProvider>(instance)?.write(pcmAt(pcm, samples)) } })
        added(Lib.vasdk.vasdk_providers_add_stt(live, id, create, destroy, event("start") { it.start() }, write,
                                                event("endOfSpeech") { it.endOfSpeech() }, event("stop") { it.stop() }, null), id)
    }

    fun tts(id: String, make: (sink: Sink, sampleRate: Int) -> TtsProvider) {
        val (create, destroy) = lifecycle(id, make)
        val synthesize = keep(TtsSynthesizeFn { instance, text ->
            if (guarded("tts $id", false) { of<TtsProvider>(instance)?.synthesize(text.utf8()) ?: false }) 1 else 0
        })
        val cancel = keep(InstanceFn { instance -> guarded("tts $id cancel") { of<TtsProvider>(instance)?.cancel() } })
        added(Lib.vasdk.vasdk_providers_add_tts(live, id, create, destroy, synthesize, cancel, null), id)
    }

    fun turn(id: String, make: (sink: Sink, sampleRate: Int) -> TurnProvider) {
        val (create, destroy) = lifecycle(id, make)
        val evaluate = keep(TurnFn { instance, pcm, samples ->
            guarded("turn $id", 0f) { of<TurnProvider>(instance)?.evaluate(pcmAt(pcm, samples)) ?: 0f }
        })
        added(Lib.vasdk.vasdk_providers_add_turn(live, id, create, destroy, evaluate, null), id)
    }

    fun wakeWord(id: String, make: (sink: Sink, sampleRate: Int) -> WakeWordProvider) {
        val (create, destroy) = lifecycle(id, make)
        val process = keep(WakeWordFn { instance, pcm, samples, phrase ->
            val found = guarded<Pair<String, Float>?>("wake word $id", null) { of<WakeWordProvider>(instance)?.process(pcmAt(pcm, samples)) }
            if (found == null) 0f else {
                Lib.vasdk.vasdk_result_set(phrase, found.first)
                found.second
            }
        })
        added(Lib.vasdk.vasdk_providers_add_wake_word(live, id, create, destroy, process, null), id)
    }

    fun audioDsp(id: String, make: (sink: Sink, sampleRate: Int) -> AudioDspProvider) {
        val (create, destroy) = lifecycle(id, make)
        val process = keep(AudioDspFn { instance, input, output, samples ->
            val changed = guarded<ShortArray?>("audio dsp $id", null) { of<AudioDspProvider>(instance)?.process(pcmAt(input, samples)) }
            if (changed != null && output != null && changed.size.toLong() == samples.toLong()) output.write(0, changed, 0, changed.size)
        })
        added(Lib.vasdk.vasdk_providers_add_audio_dsp(live, id, create, destroy, process, null), id)
    }

    fun speakerEmbedding(id: String, make: (sink: Sink, sampleRate: Int) -> SpeakerEmbeddingProvider) {
        val (create, destroy) = lifecycle(id, make)
        val embed = keep(SpeakerEmbeddingFn { instance, pcm, samples, voicePrint ->
            val values = guarded<FloatArray?>("speaker embedding $id", null) { of<SpeakerEmbeddingProvider>(instance)?.embed(pcmAt(pcm, samples)) }
            if (values != null && values.isNotEmpty()) Lib.vasdk.vasdk_result_set_floats(voicePrint, values, SizeT(values.size.toLong()))
        })
        added(Lib.vasdk.vasdk_providers_add_speaker_embedding(live, id, create, destroy, embed, null), id)
    }

    fun sentenceEncoder(id: String, make: (sink: Sink, sampleRate: Int) -> SentenceEncoderProvider) {
        val (create, destroy) = lifecycle(id, make)
        val encode = keep(InstanceTextFn { instance, sentence, vector ->
            val values = guarded<FloatArray?>("sentence encoder $id", null) { of<SentenceEncoderProvider>(instance)?.encode(sentence.utf8()) }
            if (values != null && values.isNotEmpty()) Lib.vasdk.vasdk_result_set_floats(vector, values, SizeT(values.size.toLong()))
        })
        added(Lib.vasdk.vasdk_providers_add_sentence_encoder(live, id, create, destroy, encode, null), id)
    }

    fun entityModel(id: String, make: (sink: Sink, sampleRate: Int) -> EntityModelProvider) {
        val (create, destroy) = lifecycle(id, make)
        val extract = keep(InstanceTextFn { instance, request, spans ->
            val parsed = parseJson(request.utf8()).jsonObject
            val words = parsed["words"]?.jsonArray?.map { it.jsonPrimitive.content } ?: emptyList()
            val labels = parsed["labels"]?.jsonArray?.map { label ->
                val fields = label.jsonObject
                EntityLabel(fields.text("entity"), fields.text("label"), fields.text("description"),
                            fields["examples"]?.jsonArray?.map { it.jsonPrimitive.content } ?: emptyList())
            } ?: emptyList()
            val sensitive = parsed["sensitive"]?.jsonPrimitive?.booleanOrNull ?: false
            val found = guarded("entity model $id", emptyList()) {
                of<EntityModelProvider>(instance)?.extract(words, labels, sensitive) ?: emptyList()
            }
            val json = JsonArray(found.map { span ->
                JsonObject(mapOf("entity" to JsonPrimitive(span.entity), "start" to JsonPrimitive(span.start),
                                 "length" to JsonPrimitive(span.length), "score" to JsonPrimitive(span.score)))
            })
            Lib.vasdk.vasdk_result_set(spans, json.toString())
        })
        added(Lib.vasdk.vasdk_providers_add_entity_model(live, id, create, destroy, extract, null), id)
    }

    fun llm(id: String, make: (sink: Sink, sampleRate: Int) -> LlmProvider) {
        val (create, destroy) = lifecycle(id, make)
        val complete = keep(InstanceTextFn { instance, request, reply ->
            try {
                val messages = parseJson(request.utf8()).jsonObject["messages"]?.jsonArray?.map {
                    LlmMessage(it.jsonObject.text("role"), it.jsonObject.text("content"))
                } ?: emptyList()
                val provider = of<LlmProvider>(instance) ?: throw VasdkException("no LLM instance")
                Lib.vasdk.vasdk_result_set(reply, provider.complete(messages))
            } catch (error: Throwable) {
                Lib.vasdk.vasdk_result_fail(reply, error.message ?: error.javaClass.simpleName)
            }
        })
        val cancel = keep(InstanceFn { instance -> guarded("llm $id cancel") { of<LlmProvider>(instance)?.cancel() } })
        added(Lib.vasdk.vasdk_providers_add_llm(live, id, create, destroy, complete, cancel, null), id)
    }

    /** After every session and runtime using them. */
    override fun close() {
        handle?.let { Lib.vasdk.vasdk_providers_free(it) }
        handle = null
    }
}

private fun JsonObject.text(key: String): String = (this[key] as? JsonPrimitive)?.content ?: ""
