// The C API (VASDK/include/VASDK/c/vasdk.h) as JNA sees it: every function, every callback type.
// The public classes (Vasdk, Registry, Agent, Conversation, Providers, Session, Runtime) wrap it.
@file:Suppress("FunctionName")

package com.vasdk

import com.sun.jna.Callback
import com.sun.jna.CallbackThreadInitializer
import com.sun.jna.IntegerType
import com.sun.jna.Library
import com.sun.jna.Native
import com.sun.jna.Pointer
import com.sun.jna.ptr.ByReference
import com.sun.jna.ptr.IntByReference
import com.sun.jna.ptr.PointerByReference
import java.io.File

/** size_t, the platform's width. */
class SizeT(value: Long = 0) : IntegerType(Native.SIZE_T_SIZE, value, true) {
    constructor() : this(0)
    // Kotlin's Number asks for these; java.lang.Number's defaults narrow the int value
    override fun toByte(): Byte = toInt().toByte()
    override fun toShort(): Short = toInt().toShort()
}

/** size_t*, for the lengths VASDK writes back. */
class SizeTByReference : ByReference(Native.SIZE_T_SIZE) {
    val value: Long get() = if (Native.SIZE_T_SIZE == 8) pointer.getLong(0) else pointer.getInt(0).toLong() and 0xffffffffL
}

internal const val FORMAT_BINARY = 2

// Callbacks: strings arrive as pointers, read as UTF-8 (Pointer.utf8()).
internal fun interface StepFn : Callback { fun invoke(paramsJson: Pointer, turn: Pointer, user: Pointer?) }
internal fun interface StepMessageFn : Callback { fun invoke(params: Pointer?, length: SizeT, turn: Pointer, user: Pointer?) }
internal fun interface ToolFn : Callback { fun invoke(argsJson: Pointer, result: Pointer, user: Pointer?) }
internal fun interface EnvelopeBeforeFn : Callback { fun invoke(turn: Pointer, user: Pointer?): Int }
internal fun interface EnvelopeAfterFn : Callback { fun invoke(turn: Pointer, user: Pointer?) }
internal fun interface PageEnterFn : Callback { fun invoke(paramsJson: Pointer, turn: Pointer, user: Pointer?) }
internal fun interface PageTurnFn : Callback { fun invoke(paramsJson: Pointer, turn: Pointer, user: Pointer?): Int }
internal fun interface PageEnterMessageFn : Callback { fun invoke(params: Pointer?, length: SizeT, turn: Pointer, user: Pointer?) }
internal fun interface PageTurnMessageFn : Callback { fun invoke(params: Pointer?, length: SizeT, turn: Pointer, user: Pointer?): Int }
internal fun interface ExtractorFn : Callback { fun invoke(requestJson: Pointer, result: Pointer, user: Pointer?) }
internal fun interface MessageFn : Callback { fun invoke(data: Pointer?, length: SizeT, user: Pointer?) }
internal fun interface AudioFn : Callback { fun invoke(pcm: Pointer?, samples: SizeT, user: Pointer?) }
internal fun interface ContextFn : Callback { fun invoke(result: Pointer, user: Pointer?) }
internal fun interface AudioProcessorFn : Callback { fun invoke(pcm: Pointer?, samples: SizeT, sampleRate: Int, user: Pointer?) }
internal fun interface TextProcessorFn : Callback { fun invoke(text: Pointer, result: Pointer, user: Pointer?) }
internal fun interface CreateFn : Callback { fun invoke(sink: Pointer, sampleRate: Int, user: Pointer?): Pointer? }
internal fun interface InstanceFn : Callback { fun invoke(instance: Pointer?) }
internal fun interface VadFn : Callback { fun invoke(instance: Pointer?, pcm: Pointer?, samples: SizeT): Float }
internal fun interface SttWriteFn : Callback { fun invoke(instance: Pointer?, pcm: Pointer?, samples: SizeT) }
internal fun interface TtsSynthesizeFn : Callback { fun invoke(instance: Pointer?, text: Pointer): Int }
internal fun interface TurnFn : Callback { fun invoke(instance: Pointer?, pcm: Pointer?, samples: SizeT): Float }
internal fun interface WakeWordFn : Callback { fun invoke(instance: Pointer?, pcm: Pointer?, samples: SizeT, phrase: Pointer): Float }
internal fun interface InstanceTextFn : Callback { fun invoke(instance: Pointer?, text: Pointer, result: Pointer) }

internal interface VasdkLibrary : Library {
    fun vasdk_free(pointer: Pointer?)
    fun vasdk_version(): String
    fun vasdk_set_log_level(level: String): Int

    fun vasdk_registry_create(): Pointer
    fun vasdk_registry_free(registry: Pointer)
    fun vasdk_registry_add_step(registry: Pointer, paramsType: String, step: StepFn, user: Pointer?): Int
    fun vasdk_registry_add_step_message(registry: Pointer, paramsType: String, format: Int, step: StepMessageFn, user: Pointer?): Int
    fun vasdk_registry_add_tool(registry: Pointer, name: String, description: String, tool: ToolFn, user: Pointer?): Int
    fun vasdk_registry_add_envelope(registry: Pointer, name: String, before: EnvelopeBeforeFn?, after: EnvelopeAfterFn?, user: Pointer?): Int
    fun vasdk_registry_add_page_type(registry: Pointer, name: String, paramsType: String?, onEnter: PageEnterFn?, onTurn: PageTurnFn?,
                                     user: Pointer?): Int
    fun vasdk_registry_add_page_type_message(registry: Pointer, name: String, paramsType: String?, format: Int,
                                             onEnter: PageEnterMessageFn?, onTurn: PageTurnMessageFn?, user: Pointer?): Int
    fun vasdk_registry_add_entity_extractor(registry: Pointer, name: String, extractor: ExtractorFn, user: Pointer?): Int
    fun vasdk_registry_add_language(registry: Pointer, pack: ByteArray, length: SizeT, format: Int, error: PointerByReference): Int
    fun vasdk_language_builtin(format: Int, length: SizeTByReference): Pointer?
    fun vasdk_result_set(result: Pointer, json: String?)
    fun vasdk_result_fail(result: Pointer, error: String?)
    fun vasdk_result_set_floats(result: Pointer, values: FloatArray, count: SizeT)

    fun vasdk_turn_text(turn: Pointer): Pointer?
    fun vasdk_turn_lookup_message(turn: Pointer, reference: String, format: Int, length: SizeTByReference): Pointer?
    fun vasdk_turn_say(turn: Pointer, text: String)
    fun vasdk_turn_goto(turn: Pointer, target: String)
    fun vasdk_turn_apply(turn: Pointer, outcome: ByteArray, length: SizeT, format: Int, error: PointerByReference): Int
    fun vasdk_turn_set_text(turn: Pointer, text: String)
    fun vasdk_turn_render(turn: Pointer, text: String): Pointer?
    fun vasdk_turn_reply_text(turn: Pointer): Pointer?
    fun vasdk_turn_flow(turn: Pointer): Pointer?
    fun vasdk_turn_page(turn: Pointer): Pointer?

    fun vasdk_agent_load(path: String, registry: Pointer?, error: PointerByReference): Pointer?
    fun vasdk_agent_parse(data: ByteArray, length: SizeT, format: Int, registry: Pointer?, error: PointerByReference): Pointer?
    fun vasdk_agent_proto_files(agent: Pointer, length: SizeTByReference): Pointer?
    fun vasdk_agent_free(agent: Pointer)

    fun vasdk_conversation_create(agent: Pointer): Pointer?
    fun vasdk_conversation_free(conversation: Pointer)
    fun vasdk_conversation_send(conversation: Pointer, input: ByteArray, length: SizeT, format: Int, replyLength: SizeTByReference,
                                error: PointerByReference): Pointer?
    fun vasdk_conversation_set_param_message(conversation: Pointer, name: String, value: ByteArray, length: SizeT, format: Int,
                                             error: PointerByReference): Int
    fun vasdk_conversation_lookup_message(conversation: Pointer, reference: String, format: Int, length: SizeTByReference): Pointer?
    fun vasdk_conversation_snapshot_message(conversation: Pointer, format: Int, length: SizeTByReference): Pointer?
    fun vasdk_conversation_resume_message(conversation: Pointer, snapshot: ByteArray, length: SizeT, format: Int,
                                          replyLength: SizeTByReference, resumed: IntByReference, error: PointerByReference): Pointer?
    fun vasdk_conversation_set_entity_values_message(conversation: Pointer, values: ByteArray, length: SizeT, format: Int,
                                                     error: PointerByReference): Int
    fun vasdk_conversation_set_context_message(conversation: Pointer, context: ByteArray, length: SizeT, format: Int,
                                               error: PointerByReference): Int
    fun vasdk_conversation_reply_interrupted(conversation: Pointer)
    fun vasdk_conversation_no_input_ms(conversation: Pointer): Int
    fun vasdk_conversation_next_scheduled_trigger_ms(conversation: Pointer): Long
    fun vasdk_conversation_trigger_state(conversation: Pointer): Pointer?
    fun vasdk_conversation_restore_trigger_state(conversation: Pointer, state: String): Int
    fun vasdk_run_test_file(agent: Pointer, path: String, report: PointerByReference): Int
    fun vasdk_run_test_message(agent: Pointer, testCase: ByteArray, length: SizeT, format: Int, report: PointerByReference): Int

    fun vasdk_providers_create(): Pointer
    fun vasdk_providers_free(providers: Pointer)
    fun vasdk_sink_text(sink: Pointer, text: String, isFinal: Int)
    fun vasdk_sink_audio(sink: Pointer, pcm: ShortArray, samples: SizeT)
    fun vasdk_sink_done(sink: Pointer)
    fun vasdk_sink_error(sink: Pointer, message: String)
    fun vasdk_providers_add_vad(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, detect: VadFn, user: Pointer?): Int
    fun vasdk_providers_add_stt(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, start: InstanceFn?,
                                write: SttWriteFn, endOfSpeech: InstanceFn?, stop: InstanceFn?, user: Pointer?): Int
    fun vasdk_providers_add_tts(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, synthesize: TtsSynthesizeFn,
                                cancel: InstanceFn?, user: Pointer?): Int
    fun vasdk_providers_add_turn(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, evaluate: TurnFn, user: Pointer?): Int
    fun vasdk_providers_add_wake_word(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, process: WakeWordFn,
                                      user: Pointer?): Int
    fun vasdk_providers_add_sentence_encoder(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, encode: InstanceTextFn,
                                             user: Pointer?): Int
    fun vasdk_providers_add_entity_model(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, extract: InstanceTextFn,
                                         user: Pointer?): Int
    fun vasdk_providers_add_llm(providers: Pointer, id: String, create: CreateFn, destroy: InstanceFn?, complete: InstanceTextFn,
                                cancel: InstanceFn?, user: Pointer?): Int

    fun vasdk_session_create_message(configPath: String, providers: Pointer, agent: Pointer?, options: ByteArray, length: SizeT, format: Int,
                                     error: PointerByReference): Pointer?
    fun vasdk_session_free(session: Pointer)
    fun vasdk_session_on_event_message(session: Pointer, onEvent: MessageFn, format: Int, user: Pointer?)
    fun vasdk_session_on_audio(session: Pointer, onAudio: AudioFn, user: Pointer?)
    fun vasdk_session_resume_message(session: Pointer, snapshot: ByteArray, length: SizeT, format: Int, error: PointerByReference): Int
    fun vasdk_session_start(session: Pointer, error: PointerByReference): Int
    fun vasdk_session_stop(session: Pointer)
    fun vasdk_session_input_rate(session: Pointer): Int
    fun vasdk_session_output_rate(session: Pointer): Int
    fun vasdk_session_push_audio(session: Pointer, pcm: ShortArray, samples: SizeT)
    fun vasdk_session_send(session: Pointer, command: ByteArray, length: SizeT, format: Int, error: PointerByReference): Int
    fun vasdk_session_set_network_available(session: Pointer, available: Int)
    fun vasdk_session_snapshot_message(session: Pointer, format: Int, length: SizeTByReference): Pointer?
    fun vasdk_session_add_tool_message(session: Pointer, declaration: ByteArray, length: SizeT, format: Int, tool: ToolFn, user: Pointer?,
                                       error: PointerByReference): Int
    fun vasdk_session_set_context_message(session: Pointer, context: ByteArray, length: SizeT, format: Int, error: PointerByReference): Int
    fun vasdk_session_on_context(session: Pointer, onContext: ContextFn, user: Pointer?)
    fun vasdk_session_describe(session: Pointer): Pointer?
    fun vasdk_session_using_fallbacks(session: Pointer): Int
    fun vasdk_session_next_scheduled_trigger_ms(session: Pointer): Long
    fun vasdk_session_trigger_state(session: Pointer): Pointer?
    fun vasdk_session_restore_trigger_state(session: Pointer, state: String)
    fun vasdk_session_add_audio_processor(session: Pointer, slot: Int, processor: AudioProcessorFn, user: Pointer?): Int
    fun vasdk_session_add_text_processor(session: Pointer, slot: Int, processor: TextProcessorFn, user: Pointer?): Int

    fun vasdk_runtime_create(configPath: String, providers: Pointer, modelsDir: String?, maxSessions: Int, error: PointerByReference): Pointer?
    fun vasdk_runtime_free(runtime: Pointer)
    fun vasdk_runtime_session_create(runtime: Pointer, agent: Pointer?, options: ByteArray, length: SizeT, format: Int,
                                     error: PointerByReference): Pointer?
    fun vasdk_runtime_active_sessions(runtime: Pointer): SizeT
    fun vasdk_runtime_idle_provider_sets(runtime: Pointer): SizeT
}

/** The vasdk_providers library: every provider built in (ProviderCatalogC.h). */
internal interface ProvidersLibrary : Library {
    fun vasdk_providers_add_all(providers: Pointer)
    fun vasdk_providers_set_log_level(level: String): Int
}

internal object Lib {
    private val options = mapOf(Library.OPTION_STRING_ENCODING to "UTF-8")

    /** VASDK_LIBRARY (a path), else the system property vasdk.library, else "VASDK" on the library path (Android: libVASDK.so). */
    val path: String = System.getenv("VASDK_LIBRARY")?.takeIf { it.isNotEmpty() } ?: System.getProperty("vasdk.library") ?: "VASDK"

    val vasdk: VasdkLibrary by lazy { Native.load(path, VasdkLibrary::class.java, options) }

    /** VASDK_PROVIDERS_LIBRARY, else beside VASDK in a build tree, else "vasdk_providers" on the library path. */
    val providers: ProvidersLibrary by lazy {
        val explicit = System.getenv("VASDK_PROVIDERS_LIBRARY")?.takeIf { it.isNotEmpty() }
        val beside = File(path).absoluteFile.parentFile?.parentFile
            ?.let { File(it, "VASDK_3P_IMPL/ProviderCatalog/" + System.mapLibraryName("vasdk_providers")) }
        val name = explicit ?: beside?.takeIf { it.exists() }?.path ?: "vasdk_providers"
        Native.load(name, ProvidersLibrary::class.java, options)
    }

    private val threads = CallbackThreadInitializer(true, false, "vasdk")

    /** A callback C may call from its own threads: those stay attached to the JVM (no attach per call). */
    fun <C : Callback> attached(callback: C): C {
        Native.setCallbackThreadInitializer(callback, threads)
        return callback
    }
}

internal fun Pointer?.utf8(): String = this?.getString(0, "UTF-8") ?: ""

/** Text VASDK returned (malloc'd), read and freed. */
internal fun taken(pointer: Pointer?): String {
    if (pointer == null) return ""
    try {
        return pointer.getString(0, "UTF-8")
    } finally {
        Lib.vasdk.vasdk_free(pointer)
    }
}

/** A message VASDK returned, `length` bytes, read and freed. */
internal fun takenBytes(pointer: Pointer?, length: SizeTByReference): ByteArray {
    if (pointer == null) return ByteArray(0)
    try {
        return pointer.getByteArray(0, length.value.toInt())
    } finally {
        Lib.vasdk.vasdk_free(pointer)
    }
}

/** Bytes VASDK gives a callback: valid only during it. */
internal fun bytesAt(data: Pointer?, length: SizeT): ByteArray =
    if (data == null || length.toLong() == 0L) ByteArray(0) else data.getByteArray(0, length.toInt())

internal fun pcmAt(pcm: Pointer?, samples: SizeT): ShortArray =
    if (pcm == null || samples.toLong() == 0L) ShortArray(0) else pcm.getShortArray(0, samples.toInt())

/** Throws the error VASDK set when `ok` is false. */
internal fun check(ok: Boolean, error: PointerByReference) {
    if (!ok) throw VasdkException(taken(error.value).ifEmpty { "failed" })
}

internal fun size(bytes: ByteArray) = SizeT(bytes.size.toLong())
