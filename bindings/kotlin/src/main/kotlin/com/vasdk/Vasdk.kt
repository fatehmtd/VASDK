// VASDK from Kotlin: the library's version and logs, the language pack, flow tests, and what a
// step, page type or envelope written in Kotlin gets (Turn) and is registered with (Registry).
package com.vasdk

import com.google.protobuf.ListValue
import com.google.protobuf.NullValue
import com.google.protobuf.Struct
import com.google.protobuf.Value
import com.sun.jna.Pointer
import com.sun.jna.ptr.PointerByReference
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import vasdk.flow.AgentOuterClass.Choice
import vasdk.flow.ExpectationOuterClass.Expectation
import vasdk.flow.StepOutcomeOuterClass.StepAsk
import vasdk.flow.StepOutcomeOuterClass.StepOutcome
import vasdk.flow.TestCase.FlowTestCase
import vasdk.nlu.LanguageOuterClass.Language
import java.io.Closeable

/** A VASDK call failed; the message says why (an agent's errors say where). */
class VasdkException(message: String) : RuntimeException(message)

object Vasdk {
    fun version(): String = Lib.vasdk.vasdk_version()

    /** The library's C API version (VASDK_C_API_VERSION). */
    fun apiVersion(): Int = Lib.vasdk.vasdk_c_api_version()

    /**
     * How much VASDK logs to the console: "trace", "debug", "info" (the default), "warn", "error"
     * or "off". `providers`: the vasdk_providers library's too (loading it).
     */
    fun setLogLevel(level: String, providers: Boolean = false) {
        if (Lib.vasdk.vasdk_set_log_level(level) == 0) throw VasdkException("unknown log level \"$level\"")
        if (providers) Lib.providers.vasdk_providers_set_log_level(level)
    }

    /** VASDK's own en-US pack: to copy and change, or to start another language from. */
    fun builtinLanguage(): Language {
        val length = SizeTByReference()
        return Language.parseFrom(takenBytes(Lib.vasdk.vasdk_language_builtin(FORMAT_BINARY, length), length))
    }

    /** Plays a FlowTestCase file: whether it passed, and a line per difference. */
    fun runTestFile(agent: Agent, path: String): Pair<Boolean, String> {
        val report = PointerByReference()
        val passed = Lib.vasdk.vasdk_run_test_file(agent.handle, path, report) != 0
        return passed to taken(report.value)
    }

    /** The same, a FlowTestCase from memory. */
    fun runTest(agent: Agent, case: FlowTestCase): Pair<Boolean, String> {
        val bytes = case.toByteArray()
        val report = PointerByReference()
        val passed = Lib.vasdk.vasdk_run_test_message(agent.handle, bytes, size(bytes), FORMAT_BINARY, report) != 0
        return passed to taken(report.value)
    }
}

//  JSON and google.protobuf.Value, both ways: tools, extractors and LLMs speak JSON; values cross as Value

internal fun JsonElement.toValue(): Value = when (this) {
    is JsonNull -> Value.newBuilder().setNullValue(NullValue.NULL_VALUE).build()
    is JsonObject -> Value.newBuilder().setStructValue(toStruct()).build()
    is JsonArray -> Value.newBuilder().setListValue(ListValue.newBuilder().addAllValues(map { it.toValue() })).build()
    is JsonPrimitive -> when {
        isString -> Value.newBuilder().setStringValue(content).build()
        booleanOrNull != null -> Value.newBuilder().setBoolValue(booleanOrNull!!).build()
        else -> Value.newBuilder().setNumberValue(doubleOrNull ?: 0.0).build()
    }
}

internal fun JsonObject.toStruct(): Struct = Struct.newBuilder().putAllFields(mapValues { it.value.toValue() }).build()

/** A Value as JSON: its numbers whole when they are (3, not 3.0), as JSON reads them. */
fun Value.toJson(): JsonElement = when (kindCase) {
    Value.KindCase.STRUCT_VALUE -> JsonObject(structValue.fieldsMap.mapValues { it.value.toJson() })
    Value.KindCase.LIST_VALUE -> JsonArray(listValue.valuesList.map { it.toJson() })
    Value.KindCase.STRING_VALUE -> JsonPrimitive(stringValue)
    Value.KindCase.BOOL_VALUE -> JsonPrimitive(boolValue)
    Value.KindCase.NUMBER_VALUE ->
        if (numberValue % 1.0 == 0.0 && kotlin.math.abs(numberValue) < 1e15) JsonPrimitive(numberValue.toLong()) else JsonPrimitive(numberValue)
    else -> JsonNull
}

/** A Kotlin value (null, Boolean, Number, String, List, Map, or JSON) as JSON. */
fun jsonOf(value: Any?): JsonElement = when (value) {
    null -> JsonNull
    is JsonElement -> value
    is Boolean -> JsonPrimitive(value)
    is Number -> JsonPrimitive(value)
    is String -> JsonPrimitive(value)
    is Map<*, *> -> JsonObject(value.entries.associate { it.key.toString() to jsonOf(it.value) })
    is Iterable<*> -> JsonArray(value.map { jsonOf(it) })
    else -> JsonPrimitive(value.toString())
}

internal fun parseJson(text: String): JsonElement = if (text.isBlank()) JsonNull else Json.parseToJsonElement(text)

/** The turn a step, page type or envelope runs in; only valid inside its callback. */
class Turn internal constructor(private val handle: Pointer) {
    /** What the user said this turn. */
    val text: String get() = Lib.vasdk.vasdk_turn_text(handle).utf8()

    /** What the reply says so far. */
    val replyText: String get() = Lib.vasdk.vasdk_turn_reply_text(handle).utf8()

    val flow: String get() = taken(Lib.vasdk.vasdk_turn_flow(handle))
    val page: String get() = taken(Lib.vasdk.vasdk_turn_page(handle))

    /** The value a reference holds now ("$session.tier", "$form.day"); JsonNull when unknown. */
    fun lookup(reference: String): JsonElement {
        val length = SizeTByReference()
        return Value.parseFrom(takenBytes(Lib.vasdk.vasdk_turn_lookup_message(handle, reference, FORMAT_BINARY, length), length)).toJson()
    }

    fun say(text: String) = Lib.vasdk.vasdk_turn_say(handle, text)

    /** Moves to `target` once the callback returns: a page, "flow:<name>", END_FLOW, END_SESSION or CURRENT_PAGE. */
    fun goTo(target: String) = Lib.vasdk.vasdk_turn_goto(handle, target)

    /** Changes what the rest of the turn reads as the user's words (an envelope's before: redact them). */
    fun setText(text: String) = Lib.vasdk.vasdk_turn_set_text(handle, text)

    /** A template with its references filled in ("Welcome back, $session.name."), as a say does. */
    fun render(text: String): String = taken(Lib.vasdk.vasdk_turn_render(handle, text))

    /** Asks a question, as an ask step: an answer is expected. */
    fun ask(question: String, choices: List<String> = emptyList(), expect: Expectation? = null) {
        val ask = StepAsk.newBuilder().setQuestion(question)
        choices.forEach { ask.addChoices(Choice.newBuilder().setLabel(it)) }
        expect?.let { ask.setExpect(it) }
        apply(StepOutcome.newBuilder().setAsk(ask).build())
    }

    /** What a step does, all at once: values to set and unset, texts to say, a question, a target. */
    fun apply(outcome: StepOutcome) {
        val bytes = outcome.toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_turn_apply(handle, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }
}

/**
 * Steps, tools, envelopes, page types, entity extractors and languages written in Kotlin, for the
 * agents loaded with it. Callbacks run on the conversation's thread (a session's pipeline thread):
 * keep them quick, and hop to your own dispatcher before touching UI. An exception thrown by one
 * is logged; a tool's fails the call.
 */
class Registry : Closeable {
    internal var handle: Pointer? = Lib.vasdk.vasdk_registry_create()
    private val callbacks = mutableListOf<Any>()   // C holds them for as long as the registry lives

    private fun <C : com.sun.jna.Callback> keep(callback: C): C = Lib.attached(callback).also { synchronized(callbacks) { callbacks += it } }
    private val live: Pointer get() = handle ?: throw VasdkException("registry closed")

    /** A step for custom { [type.googleapis.com/<paramsType>] {...} }, its params as their bytes: parse them with the type's class. */
    fun step(paramsType: String, step: (params: ByteArray, turn: Turn) -> Unit) {
        val callback = keep(StepMessageFn { params, length, turn, _ -> guarded("step $paramsType") { step(bytesAt(params, length), Turn(turn)) } })
        if (Lib.vasdk.vasdk_registry_add_step_message(live, paramsType, FORMAT_BINARY, callback, null) == 0)
            throw VasdkException("step $paramsType not added")
    }

    /** The same, its params as JSON: for a type only the agent's own .proto declares. */
    fun stepJson(paramsType: String, step: (params: JsonElement, turn: Turn) -> Unit) {
        val callback = keep(StepFn { params, turn, _ -> guarded("step $paramsType") { step(parseJson(params.utf8()), Turn(turn)) } })
        if (Lib.vasdk.vasdk_registry_add_step(live, paramsType, callback, null) == 0) throw VasdkException("step $paramsType not added")
    }

    /** A tool for call_tool steps: its args as a JSON object, its result as JSON. Throwing fails the call (tool-error). */
    fun tool(name: String, description: String = "", tool: (args: JsonObject) -> JsonElement) {
        val callback = keep(toolCallback(tool))
        if (Lib.vasdk.vasdk_registry_add_tool(live, name, description, callback, null) == 0) throw VasdkException("tool $name not added")
    }

    /** An envelope for envelopes: ["<name>"]: before() runs before the page's routes and returns true when it answered the turn; after() once the reply is ready. */
    fun envelope(name: String, before: ((Turn) -> Boolean)? = null, after: ((Turn) -> Unit)? = null) {
        val beforeCallback = before?.let { keep(EnvelopeBeforeFn { turn, _ -> if (guarded("envelope $name", false) { it(Turn(turn)) }) 1 else 0 }) }
        val afterCallback = after?.let { keep(EnvelopeAfterFn { turn, _ -> guarded("envelope $name") { it(Turn(turn)) } }) }
        if (Lib.vasdk.vasdk_registry_add_envelope(live, name, beforeCallback, afterCallback, null) == 0)
            throw VasdkException("envelope $name not added")
    }

    /**
     * A page type for pages with type: "<name>": onEnter when the page is entered, onTurn for each
     * turn on it (true: it answered the turn; false: the page's routes get it). Params as their
     * bytes, empty for a type without params (paramsType null).
     */
    fun pageType(name: String, paramsType: String? = null, onEnter: ((params: ByteArray, turn: Turn) -> Unit)? = null,
                 onTurn: ((params: ByteArray, turn: Turn) -> Boolean)? = null) {
        val enter = onEnter?.let {
            keep(PageEnterMessageFn { params, length, turn, _ -> guarded("page type $name") { it(bytesAt(params, length), Turn(turn)) } })
        }
        val turn = onTurn?.let {
            keep(PageTurnMessageFn { params, length, turn, _ ->
                if (guarded("page type $name", false) { it(bytesAt(params, length), Turn(turn)) }) 1 else 0
            })
        }
        if (Lib.vasdk.vasdk_registry_add_page_type_message(live, name, paramsType, FORMAT_BINARY, enter, turn, null) == 0)
            throw VasdkException("page type $name not added")
    }

    /** The same, the params as JSON: for a type only the agent's own .proto declares. */
    fun pageTypeJson(name: String, paramsType: String? = null, onEnter: ((params: JsonElement, turn: Turn) -> Unit)? = null,
                     onTurn: ((params: JsonElement, turn: Turn) -> Boolean)? = null) {
        val enter = onEnter?.let {
            keep(PageEnterFn { params, turn, _ -> guarded("page type $name") { it(parseJson(params.utf8()), Turn(turn)) } })
        }
        val turn = onTurn?.let {
            keep(PageTurnFn { params, turn, _ -> if (guarded("page type $name", false) { it(parseJson(params.utf8()), Turn(turn)) }) 1 else 0 })
        }
        if (Lib.vasdk.vasdk_registry_add_page_type(live, name, paramsType, enter, turn, null) == 0)
            throw VasdkException("page type $name not added")
    }

    /**
     * Finds the values of code { extractor: "<name>" } entities in the user's words (a database, the
     * phone's contacts): gets the words and the text, gives what it found:
     * [{"start": 2, "length": 1, "value": "mom-mobile", "spoken": "Mom", "attributes": {...}}].
     */
    fun entityExtractor(name: String, extractor: (words: List<String>, text: String) -> JsonArray) {
        val callback = keep(ExtractorFn { request, result, _ ->
            val parsed = parseJson(request.utf8()) as? JsonObject ?: JsonObject(emptyMap())
            val words = (parsed["words"] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList()
            val text = (parsed["text"] as? JsonPrimitive)?.content ?: ""
            val found = guarded("entity extractor $name", JsonArray(emptyList())) { extractor(words, text) }
            Lib.vasdk.vasdk_result_set(result, found.toString())
        })
        if (Lib.vasdk.vasdk_registry_add_entity_extractor(live, name, callback, null) == 0)
            throw VasdkException("entity extractor $name not added")
    }

    /** A language agents may name in locale or languages; one of the same code replaces it. */
    fun addLanguage(pack: Language) {
        val bytes = pack.toByteArray()
        val error = PointerByReference()
        check(Lib.vasdk.vasdk_registry_add_language(live, bytes, size(bytes), FORMAT_BINARY, error) != 0, error)
    }

    /** Frees it, after every agent loaded with it. */
    override fun close() {
        handle?.let { Lib.vasdk.vasdk_registry_free(it) }
        handle = null
    }
}

internal fun toolCallback(tool: (args: JsonObject) -> JsonElement) = ToolFn { args, result, _ ->
    try {
        val parsed = parseJson(args.utf8()) as? JsonObject ?: JsonObject(emptyMap())
        Lib.vasdk.vasdk_result_set(result, tool(parsed).toString())
    } catch (error: Throwable) {
        Lib.vasdk.vasdk_result_fail(result, error.message ?: error.javaClass.simpleName)
    }
}

/** Runs a callback of the app's: a throw is logged and gives `fallback`, never crosses into C. */
internal inline fun <T> guarded(where: String, fallback: T, block: () -> T): T = try {
    block()
} catch (error: Throwable) {
    System.err.println("vasdk: $where failed: $error")
    fallback
}

internal inline fun guarded(where: String, block: () -> Unit) = guarded(where, Unit, block)
