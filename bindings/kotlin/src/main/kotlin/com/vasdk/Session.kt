// Spoken sessions and the runtime many of them share (VASDK/include/VASDK/c/vasdk.h: Sessions, Runtime).
package com.vasdk

import com.google.protobuf.Struct
import com.sun.jna.Pointer
import com.sun.jna.ptr.PointerByReference
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import vasdk.engine.EngineConfigOuterClass.Backchannel
import vasdk.engine.Events.Command
import vasdk.engine.Events.EntityValuesCommand
import vasdk.engine.Events.Event
import vasdk.engine.Events.SessionParamCommand
import vasdk.engine.Events.TriggerCommand
import vasdk.engine.Events.VoicePrint
import vasdk.engine.Events.VoicePrints
import vasdk.engine.Session.SessionOptions
import vasdk.engine.Session.ToolDeclaration
import vasdk.flow.Snapshot.ConversationSnapshot
import vasdk.nlu.Entity.EntityValue
import java.io.Closeable

/** Where Session.addProcessor puts a processor of the app's (vasdk_slot). */
enum class Slot {
    /** The microphone's audio, before VAD: to read (a recorder, a level meter). */
    AFTER_INPUT,
    /** Each final transcript, before the agent. */
    AFTER_TRANSCRIPTION,
    /** Each part of a reply, before TTS. */
    BEFORE_SYNTHESIS,
    /** The reply's audio, before it plays: to read or change. */
    BEFORE_OUTPUT,
}

/**
 * A spoken conversation: the providers and the LLM an engine config names, and an agent (or none,
 * for the LLM alone). The app brings the audio (pushAudio() the user's 16-bit mono PCM, onAudio()
 * for the assistant's speech), unless the config names a webrtc peer or the device's audio
 * (providers.audio_input and audio_output).
 *
 *     val session = Session("engine.txtpb", Providers(), agent, SessionOptions.newBuilder().setId("call-1").build())
 *     session.onEvent { event -> ... }
 *     session.start()
 *     session.injectText("set a timer for five minutes")
 *
 * onEvent, onAudio, onContext, tool() and addProcessor() come before start(). Callbacks run on
 * VASDK's threads: keep them quick, and hop to your own dispatcher before touching UI.
 */
class Session private constructor(private var handle: Pointer?, private val owner: Any, private val agent: Agent?) : Closeable {
    private val callbacks = mutableListOf<Any>()   // C holds them for as long as the session lives
    private val eventFlow = MutableSharedFlow<Event>(extraBufferCapacity = 256, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    private val listeners = mutableListOf<(Event) -> Unit>()
    private val live: Pointer get() = handle ?: throw VasdkException("session closed")

    /** Every event, as it happens (transcripts, replies, state, tool calls, errors). */
    val events: SharedFlow<Event> = eventFlow.asSharedFlow()

    constructor(configPath: String, providers: Providers, agent: Agent? = null, options: SessionOptions = SessionOptions.getDefaultInstance())
        : this(create(configPath, providers, agent, options), providers, agent)

    init {
        val onEvent = keep(MessageFn { data, length, _ ->
            val event = guarded<Event?>("event", null) { Event.parseFrom(bytesAt(data, length)) } ?: return@MessageFn
            eventFlow.tryEmit(event)
            val each = synchronized(listeners) { listeners.toList() }
            each.forEach { guarded("onEvent") { it(event) } }
        })
        Lib.vasdk.vasdk_session_on_event_message(live, onEvent, FORMAT_BINARY, null)
    }

    internal companion object {
        fun create(configPath: String, providers: Providers, agent: Agent?, options: SessionOptions): Pointer {
            val bytes = options.toByteArray()
            val error = PointerByReference()
            return Lib.vasdk.vasdk_session_create_message(configPath, providers.handle ?: throw VasdkException("providers closed"),
                                                          agent?.handle, bytes, size(bytes), FORMAT_BINARY, error)
                ?: throw VasdkException(taken(error.value))
        }

        fun adopt(handle: Pointer, runtime: Runtime, agent: Agent?) = Session(handle, runtime, agent)
    }

    private fun <C : com.sun.jna.Callback> keep(callback: C): C = Lib.attached(callback).also { synchronized(callbacks) { callbacks += it } }

    val inputRate: Int get() = Lib.vasdk.vasdk_session_input_rate(live)
    val outputRate: Int get() = Lib.vasdk.vasdk_session_output_rate(live)

    /** Each event, on the session's event thread (also on `events`). */
    fun onEvent(listener: (Event) -> Unit) {
        synchronized(listeners) { listeners += listener }
    }

    /** The assistant's speech, 16-bit mono at outputRate, on a pipeline thread. */
    fun onAudio(listener: (ShortArray) -> Unit) {
        Lib.vasdk.vasdk_session_on_audio(live, keep(AudioFn { pcm, samples, _ -> guarded("onAudio") { listener(pcmAt(pcm, samples)) } }), null)
    }

    /** A tool the session's LLM may call: its args as a JSON object, its result as JSON; throwing fails it, and the LLM is told why. */
    fun tool(declaration: ToolDeclaration, tool: (args: JsonObject) -> JsonElement) {
        val bytes = declaration.toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_session_add_tool_message(live, bytes, size(bytes), FORMAT_BINARY, keep(toolCallback(tool)), null, error) != 0, error)
    }

    /** The app's state, asked for each time the agent or the LLM reads it; its keys win over setContext's. Keep it quick. */
    fun onContext(context: () -> JsonObject) {
        Lib.vasdk.vasdk_session_on_context(live, keep(ContextFn { result, _ ->
            guarded("onContext") { Lib.vasdk.vasdk_result_set(result, context().toString()) }
        }), null)
    }

    /**
     * A processor of the app's at an audio slot: AFTER_INPUT gets the microphone's audio to read;
     * BEFORE_OUTPUT gets the reply's, and what it returns (the same length) replaces it.
     */
    fun addProcessor(slot: Slot, processor: (pcm: ShortArray, sampleRate: Int) -> ShortArray?) {
        require(slot == Slot.AFTER_INPUT || slot == Slot.BEFORE_OUTPUT) { "$slot is a text slot: addTextProcessor" }
        val callback = keep(AudioProcessorFn { pcm, samples, rate, _ ->
            val changed = guarded<ShortArray?>("audio processor", null) { processor(pcmAt(pcm, samples), rate) }
            if (slot == Slot.BEFORE_OUTPUT && pcm != null && changed != null && changed.size.toLong() == samples.toLong())
                pcm.write(0, changed, 0, changed.size)
        })
        if (Lib.vasdk.vasdk_session_add_audio_processor(live, slot.ordinal, callback, null) == 0)
            throw VasdkException("no processor added at $slot (the session started)")
    }

    /** A processor of the app's at a text slot (AFTER_TRANSCRIPTION, BEFORE_SYNTHESIS): null passes the text on, a string replaces it, "" drops it. */
    fun addTextProcessor(slot: Slot, processor: (text: String) -> String?) {
        require(slot == Slot.AFTER_TRANSCRIPTION || slot == Slot.BEFORE_SYNTHESIS) { "$slot is an audio slot: addProcessor" }
        val callback = keep(TextProcessorFn { text, result, _ ->
            guarded<String?>("text processor", null) { processor(text.utf8()) }?.let { Lib.vasdk.vasdk_result_set(result, it) }
        })
        if (Lib.vasdk.vasdk_session_add_text_processor(live, slot.ordinal, callback, null) == 0)
            throw VasdkException("no processor added at $slot (the session started)")
    }

    /** Starts listening and speaking; `resume`: an earlier session's snapshot() to pick up. */
    fun start(resume: ConversationSnapshot? = null) {
        if (resume != null && resume.flow.isNotEmpty()) {
            val bytes = resume.toByteArray()
            val error = PointerByReference()
            check(Lib.vasdk.vasdk_session_resume_message(live, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
        }
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_session_start(live, error) != 0, error)
    }

    fun stop() = Lib.vasdk.vasdk_session_stop(live)

    /** The user's audio, 16-bit mono at inputRate, as it comes. Thread-safe. */
    fun pushAudio(pcm: ShortArray) {
        if (pcm.isNotEmpty()) Lib.vasdk.vasdk_session_push_audio(live, pcm, SizeT(pcm.size.toLong()))
    }

    /** Does what a Command asks; the methods below are shortcuts. Thread-safe. */
    fun send(command: Command) {
        val bytes = command.toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_session_send(live, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }

    /** A user turn by text, as if spoken. */
    fun injectText(text: String) = send(Command.newBuilder().setInjectText(text).build())

    /** Words the assistant says. */
    fun say(text: String) = send(Command.newBuilder().setSay(text).build())

    /** The user picked the last reply's choice at `index` (from 0). */
    fun selectChoice(index: Int) = send(Command.newBuilder().setSelectChoice(index).build())

    /** Stops the reply playing, as the user talking over it would. */
    fun interrupt() = send(Command.newBuilder().setInterrupt(true).build())

    /** Sets a declared $session.<name> of the agent. */
    fun setParam(name: String, value: Any?) =
        send(Command.newBuilder().setSetSessionParam(SessionParamCommand.newBuilder().setName(name).setValue(jsonOf(value).toValue())).build())

    /** Fires the agent's app_event triggers named `name`; payload is $trigger.payload. */
    fun fireTrigger(name: String, payload: JsonObject = JsonObject(emptyMap())) =
        send(Command.newBuilder().setFireTrigger(TriggerCommand.newBuilder().setName(name).setPayload(payload.toStruct())).build())

    /** Starts the conversation over: the LLM's history cleared, the agent at its start. */
    fun resetConversation() = send(Command.newBuilder().setResetConversation(true).build())

    /** The talk button held (true) or let go (false): PUSH_TO_TALK and HYBRID activation. */
    fun pushToTalk(held: Boolean) = send(Command.newBuilder().setPushToTalk(held).build())

    /** How long the assistant keeps listening after a reply; 0: not at all. */
    fun setFollowUpListenMs(ms: Int) = send(Command.newBuilder().setFollowUpListenMs(ms).build())

    /** A dynamic entity's values (the phone's contacts). */
    fun setEntityValues(name: String, values: List<EntityValue>) =
        send(Command.newBuilder().setSetEntityValues(EntityValuesCommand.newBuilder().setName(name).addAllValues(values)).build())

    /** Back-channel handling: the fields set replace the session's. */
    fun setBackchannel(backchannel: Backchannel) = send(Command.newBuilder().setSetBackchannel(backchannel).build())

    /** The next utterance enrolls `speakerId`, replacing their voice print; a speaker_enrollment event says how it went.
     *  Needs providers.speaker_embedding in the config. */
    fun enrollSpeaker(speakerId: String) = send(Command.newBuilder().setEnrollSpeaker(speakerId).build())

    /** Restores a voice print kept from speakers(). */
    fun addSpeaker(speakerId: String, voicePrint: FloatArray) =
        send(Command.newBuilder().setAddSpeaker(VoicePrint.newBuilder().setSpeakerId(speakerId).addAllEmbedding(voicePrint.asList())).build())

    /** The enrolled speakers' voice prints, by speaker id, to keep across sessions. */
    fun speakers(): Map<String, FloatArray> {
        val length = SizeTByReference()
        val prints = VoicePrints.parseFrom(takenBytes(Lib.vasdk.vasdk_session_speakers_message(live, FORMAT_BINARY, length), length))
        return prints.speakersList.associate { it.speakerId to it.embeddingList.toFloatArray() }
    }

    /** The app's state: what the agent's $context.* reads, and {context} in the LLM's instructions. Each top-level key is replaced. */
    fun setContext(context: JsonObject) = setContext(context.toStruct())

    fun setContext(context: Struct) {
        val bytes = context.toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_session_set_context_message(live, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }

    /** Down: local fallbacks speak and listen, and the LLM rests. */
    fun setNetworkAvailable(available: Boolean) = Lib.vasdk.vasdk_session_set_network_available(live, if (available) 1 else 0)

    /** Where the agent's conversation is, to start(resume) a later session there (its flow empty when there's nothing to resume). */
    fun snapshot(): ConversationSnapshot {
        val length = SizeTByReference()
        return ConversationSnapshot.parseFrom(takenBytes(Lib.vasdk.vasdk_session_snapshot_message(live, FORMAT_BINARY, length), length))
    }

    /** What the session runs: its providers, its LLM, its stages. */
    fun describe(): String = taken(Lib.vasdk.vasdk_session_describe(live))

    /** True while the network is down and the local fallbacks speak and listen. */
    fun usingFallbacks(): Boolean = Lib.vasdk.vasdk_session_using_fallbacks(live) != 0

    /** When the agent's next schedule trigger is due, in milliseconds since the epoch (an alarm to set); null for none. */
    fun nextScheduledTrigger(): Long? = Lib.vasdk.vasdk_session_next_scheduled_trigger_ms(live).takeIf { it >= 0 }

    /** What the agent's triggers have seen, as JSON: keep it to restoreTriggerState() after a restart. */
    fun triggerState(): String = taken(Lib.vasdk.vasdk_session_trigger_state(live))

    fun restoreTriggerState(state: String) = Lib.vasdk.vasdk_session_restore_trigger_state(live, state)

    /** Stops it and frees it. */
    override fun close() {
        handle?.let { Lib.vasdk.vasdk_session_free(it) }
        handle = null
    }
}

/**
 * What many sessions share: the engine config and its providers, loaded once per session running
 * at the same time (a call center's lines), not once per conversation. Close the sessions first.
 */
class Runtime(configPath: String, private val providers: Providers, modelsDir: String? = null, maxSessions: Int = 0) : Closeable {
    private var handle: Pointer? = run {
        val error = PointerByReference()
        Lib.vasdk.vasdk_runtime_create(configPath, providers.handle ?: throw VasdkException("providers closed"), modelsDir, maxSessions, error)
            ?: throw VasdkException(taken(error.value))
    }
    private val live: Pointer get() = handle ?: throw VasdkException("runtime closed")

    /** A session over the runtime's providers; fails at maxSessions. */
    fun session(agent: Agent? = null, options: SessionOptions = SessionOptions.getDefaultInstance()): Session {
        val bytes = options.toByteArray()
        val error = PointerByReference()
        val session = Lib.vasdk.vasdk_runtime_session_create(live, agent?.handle, bytes, size(bytes), FORMAT_BINARY, error)
            ?: throw VasdkException(taken(error.value))
        return Session.adopt(session, this, agent)
    }

    fun activeSessions(): Long = Lib.vasdk.vasdk_runtime_active_sessions(live).toLong()

    /** Provider sets loaded and waiting for a session. */
    fun idleProviderSets(): Long = Lib.vasdk.vasdk_runtime_idle_provider_sets(live).toLong()

    override fun close() {
        handle?.let { Lib.vasdk.vasdk_runtime_free(it) }
        handle = null
    }
}
