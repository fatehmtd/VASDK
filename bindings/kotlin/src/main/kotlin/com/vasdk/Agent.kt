// Agents and their text conversations (VASDK/include/VASDK/c/vasdk.h: Agents, Conversations).
package com.vasdk

import com.google.protobuf.Struct
import com.google.protobuf.Value
import com.sun.jna.Pointer
import com.sun.jna.ptr.IntByReference
import com.sun.jna.ptr.PointerByReference
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import vasdk.engine.Events.EntityValuesCommand
import vasdk.flow.Conversation.ConversationInput
import vasdk.flow.Conversation.ConversationReply
import vasdk.flow.Conversation.TriggerInput
import vasdk.flow.Snapshot.ConversationSnapshot
import vasdk.nlu.Entity.EntityValue
import java.io.Closeable

/** An agent, checked and compiled. Keep its registry open as long as it's in use. */
class Agent private constructor(internal val handle: Pointer, private val registry: Registry?) : Closeable {
    private var closed = false

    companion object {
        /** An agent file (.txtpb, .json or .pb; its imports and goal files beside it). */
        fun load(path: String, registry: Registry? = null): Agent {
            val error = PointerByReference()
            val handle = Lib.vasdk.vasdk_agent_load(path, registry?.handle, error) ?: throw VasdkException(taken(error.value))
            return Agent(handle, registry)
        }

        /** A vasdk.flow.Agent as text proto. */
        fun parse(text: String, registry: Registry? = null): Agent = parse(text.toByteArray(Charsets.UTF_8), 0, registry)

        /** A vasdk.flow.Agent in `format`: 0 text proto, 1 JSON, 2 binary. */
        fun parse(data: ByteArray, format: Int, registry: Registry? = null): Agent {
            val error = PointerByReference()
            val handle = Lib.vasdk.vasdk_agent_parse(data, size(data), format, registry?.handle, error)
                ?: throw VasdkException(taken(error.value))
            return Agent(handle, registry)
        }
    }

    /** The .proto files of the agent's proto_imports, as a serialized google.protobuf.FileDescriptorSet; empty without them. */
    fun protoFiles(): ByteArray {
        val length = SizeTByReference()
        return takenBytes(Lib.vasdk.vasdk_agent_proto_files(handle, length), length)
    }

    fun conversation(): Conversation = Conversation(this)

    /** After its conversations and sessions. */
    override fun close() {
        if (!closed) Lib.vasdk.vasdk_agent_free(handle)
        closed = true
    }
}

/**
 * One conversation's place in an agent, by text. Each turn gives a vasdk.flow.ConversationReply:
 * text, segments, choices, matched, intent, expects_reply, ends_session, to_llm, flow, page,
 * tool calls, triggers. Used from one thread at a time.
 */
class Conversation internal constructor(private val agent: Agent) : Closeable {
    private var handle: Pointer? = Lib.vasdk.vasdk_conversation_create(agent.handle)
    private val live: Pointer get() = handle ?: throw VasdkException("conversation closed")

    /** Any ConversationInput: start, text, no_input, select_choice, fire_trigger, check_triggers, deliver_queued. */
    fun send(input: ConversationInput): ConversationReply {
        val bytes = input.toByteArray()
        val length = SizeTByReference()
        val error = PointerByReference()
        val reply = Lib.vasdk.vasdk_conversation_send(live, bytes, size(bytes), FORMAT_BINARY, length, error)
            ?: throw VasdkException(taken(error.value).ifEmpty { "failed" })
        return ConversationReply.parseFrom(takenBytes(reply, length))
    }

    fun start(): ConversationReply = send(ConversationInput.newBuilder().setStart(true).build())
    fun say(text: String): ConversationReply = send(ConversationInput.newBuilder().setText(text).build())
    fun noInput(): ConversationReply = send(ConversationInput.newBuilder().setNoInput(true).build())
    fun selectChoice(index: Int): ConversationReply = send(ConversationInput.newBuilder().setSelectChoice(index).build())

    /** One of the agent's app_event triggers; payload is $trigger.payload. */
    fun fireTrigger(name: String, payload: JsonObject = JsonObject(emptyMap())): ConversationReply =
        send(ConversationInput.newBuilder().setFireTrigger(TriggerInput.newBuilder().setName(name).setPayload(payload.toStruct())).build())

    /** The schedule and context_change triggers whose moment came: call it now and then. */
    fun checkTriggers(): ConversationReply = send(ConversationInput.newBuilder().setCheckTriggers(true).build())

    /** A queued trigger whose moment came, between turns. */
    fun deliverQueued(): ConversationReply = send(ConversationInput.newBuilder().setDeliverQueued(true).build())

    /** Sets a declared $session.<name>. */
    fun setParam(name: String, value: Any?) {
        val bytes = jsonOf(value).toValue().toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_conversation_set_param_message(live, name, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }

    /** The value a reference holds now ("$session.tier"); JsonNull when unknown. */
    fun lookup(reference: String): JsonElement {
        val length = SizeTByReference()
        return Value.parseFrom(takenBytes(Lib.vasdk.vasdk_conversation_lookup_message(live, reference, FORMAT_BINARY, length), length)).toJson()
    }

    /** Where the conversation is, to resume() it later (sensitive values left out). */
    fun snapshot(): ConversationSnapshot {
        val length = SizeTByReference()
        return ConversationSnapshot.parseFrom(takenBytes(Lib.vasdk.vasdk_conversation_snapshot_message(live, FORMAT_BINARY, length), length))
    }

    /** Starts where a snapshot left it, instead of start(): the reply, and false when it didn't fit the agent and started over. */
    fun resume(snapshot: ConversationSnapshot): Pair<ConversationReply, Boolean> {
        val bytes = snapshot.toByteArray()
        val length = SizeTByReference()
        val resumed = IntByReference()
        val error = PointerByReference()
        val reply = Lib.vasdk.vasdk_conversation_resume_message(live, bytes, size(bytes), FORMAT_BINARY, length, resumed, error)
            ?: throw VasdkException(taken(error.value).ifEmpty { "failed" })
        return ConversationReply.parseFrom(takenBytes(reply, length)) to (resumed.value != 0)
    }

    /** A dynamic entity's values for this conversation (the times still free today, the caller's cars). */
    fun setEntityValues(name: String, values: List<EntityValue>) {
        val bytes = EntityValuesCommand.newBuilder().setName(name).addAllValues(values).build().toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_conversation_set_entity_values_message(live, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }

    /** The app's state: what $context.* reads. Each top-level key replaces the one before. */
    fun setContext(context: JsonObject) = setContext(context.toStruct())

    fun setContext(context: Struct) {
        val bytes = context.toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_conversation_set_context_message(live, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }

    /** The user cut the last reply off: a read-back cut off isn't confirmed by a plain "yes". */
    fun replyInterrupted() = Lib.vasdk.vasdk_conversation_reply_interrupted(live)

    /** How long to wait for an answer before noInput(); 0: never. */
    fun noInputMs(): Int = Lib.vasdk.vasdk_conversation_no_input_ms(live)

    /** When the next schedule trigger is due, in milliseconds since the epoch; null for none. */
    fun nextScheduledTrigger(): Long? = Lib.vasdk.vasdk_conversation_next_scheduled_trigger_ms(live).takeIf { it >= 0 }

    /** What the triggers have seen, as JSON: keep it to restoreTriggerState() after a restart. */
    fun triggerState(): String = taken(Lib.vasdk.vasdk_conversation_trigger_state(live))

    fun restoreTriggerState(state: String): Boolean = Lib.vasdk.vasdk_conversation_restore_trigger_state(live, state) != 0

    override fun close() {
        handle?.let { Lib.vasdk.vasdk_conversation_free(it) }
        handle = null
    }
}
