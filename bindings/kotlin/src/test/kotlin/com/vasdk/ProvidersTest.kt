// Providers and processors written in Kotlin, through the C API: a whole spoken session with no
// models. Typed words go to the Kotlin LLM, its reply through a Kotlin text processor to the Kotlin
// TTS and out of onAudio; pushed audio goes through the Kotlin VAD to the Kotlin STT, whose turn
// the Kotlin turn model ends. The app's context, the new commands and queries, a runtime with a
// Kotlin sentence encoder and entity model; a text conversation's snapshot, resume, entity values,
// context and triggers, a page type's turn, an entity extractor, a language, a test case from memory.
// The port of bindings/python/tests/test_providers.py.
package com.vasdk

import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.onSubscription
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import vasdk.engine.EngineConfigOuterClass.Backchannel
import vasdk.flow.TestCase.FlowTestCase
import vasdk.flow.TestCase.TestTurn
import vasdk.flow.TestCase.TurnExpectation
import vasdk.nlu.Entity.EntityValue
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ProvidersTest {
    private val events = CopyOnWriteArrayList<String>()
    private val llmRequests = CopyOnWriteArrayList<List<LlmMessage>>()
    private val spoken = CopyOnWriteArrayList<String>()
    private val sttSamples = AtomicInteger()
    private val micSamples = AtomicInteger()
    private val played = AtomicInteger()
    private val turnsAsked = AtomicInteger()
    private val created = AtomicInteger()
    private val closed = AtomicInteger()

    private fun waitFor(seconds: Double = 5.0, condition: () -> Boolean): Boolean {
        val deadline = System.nanoTime() + (seconds * 1e9).toLong()
        while (System.nanoTime() < deadline) {
            if (condition()) return true
            Thread.sleep(20)
        }
        return condition()
    }

    private fun register(providers: Providers) {
        providers.vad("kotlin") { _, _ ->
            created.incrementAndGet()
            object : VadProvider {
                override fun detect(pcm: ShortArray) = if ((pcm.maxOfOrNull { abs(it.toInt()) } ?: 0) > 1000) 0.9f else 0f
                override fun close() { closed.incrementAndGet() }
            }
        }
        providers.stt("kotlin") { sink, _ ->
            object : SttProvider {
                override fun write(pcm: ShortArray) { sttSamples.addAndGet(pcm.size) }
                override fun endOfSpeech() {
                    sink.text("what", final = false)
                    sink.text("what time is it")
                }
            }
        }
        providers.tts("kotlin") { sink, _ ->
            object : TtsProvider {
                override fun synthesize(text: String): Boolean {
                    spoken += text
                    sink.audio(ShortArray(1600) { 100 })
                    sink.done()
                    return true
                }
            }
        }
        providers.turn("kotlin") { _, _ ->
            object : TurnProvider { override fun evaluate(pcm: ShortArray) = 0.9f.also { turnsAsked.incrementAndGet() } }
        }
        providers.llm("kotlin") { _, _ ->
            object : LlmProvider {
                override fun complete(messages: List<LlmMessage>): String {
                    llmRequests += messages
                    return "You said: ${messages.last { it.role == "user" }.content}."
                }
            }
        }
    }

    private fun push(session: Session, milliseconds: Int, amplitude: Int) {
        val rate = session.inputRate
        repeat(milliseconds / 10) {
            session.pushAudio(ShortArray(rate / 100) { i -> (amplitude * sin(2 * PI * 440 * i / rate)).toInt().toShort() })
            Thread.sleep(10)
        }
    }

    private fun config(text: String): String {
        val file = Files.createTempFile("vasdk_kotlin", ".txtpb").toFile()
        file.deleteOnExit()
        file.writeText(text)
        return file.path
    }

    @Test
    fun aSpokenSessionOnKotlinProviders() {
        Vasdk.setLogLevel("warn")
        Providers(builtIn = false).use { providers ->
            register(providers)
            val path = config("""providers { vad { id: "kotlin" } stt { id: "kotlin" } tts { id: "kotlin" } turn { id: "kotlin" } llm { id: "kotlin" } }""")
            val options = vasdk.engine.Session.SessionOptions.newBuilder().setSystemPrompt("Be brief. The user is in {context}.").build()
            Session(path, providers, options = options).use { session ->
                session.onEvent { events += it.toString() }
                session.onAudio { played.addAndGet(it.size) }
                session.onContext { JsonObject(mapOf("city" to JsonPrimitive("Paris"))) }
                session.addProcessor(Slot.AFTER_INPUT) { pcm, _ -> micSamples.addAndGet(pcm.size); null }
                session.addProcessor(Slot.BEFORE_OUTPUT) { pcm, _ -> pcm }
                session.addTextProcessor(Slot.BEFORE_SYNTHESIS) { text -> if ("You said" in text) text.replace("You said", "I heard") else null }
                assertFailsWith<IllegalArgumentException> { session.addTextProcessor(Slot.AFTER_INPUT) { it } }
                session.start()
                assertFailsWith<VasdkException> { session.addTextProcessor(Slot.AFTER_TRANSCRIPTION) { null } }

                session.injectText("hello")
                assertTrue(waitFor { spoken.any { "I heard: hello." in it } }, "spoken: $spoken")
                assertTrue(waitFor { played.get() >= 1600 }, "played ${played.get()}")
                val system = llmRequests.first().first()
                assertTrue(system.role == "system" && "Be brief." in system.content && "Paris" in system.content, system.toString())

                // The events flow carries them too: subscribed first, then the words that make one
                runBlocking { withTimeout(5000) { session.events.onSubscription { session.say("One more thing.") }.first() } }

                push(session, 300, 0)
                push(session, 1200, 8000)
                push(session, 1500, 0)
                assertTrue(waitFor { events.any { "what time is it" in it } }, "no transcript event")
                assertTrue(sttSamples.get() > 0 && micSamples.get() > 0 && turnsAsked.get() > 0)
                assertTrue(waitFor { llmRequests.any { request -> request.any { "what time is it" in it.content } } })

                assertTrue("kotlin" in session.describe())
                assertTrue(!session.usingFallbacks())
                assertNull(session.nextScheduledTrigger())
                session.resetConversation()
                session.pushToTalk(true)
                session.pushToTalk(false)
                session.setFollowUpListenMs(3000)
                session.setBackchannel(Backchannel.newBuilder().setEnabled(false).setDuckMs(1200).build())
                session.setEntityValues("contact", listOf(EntityValue.newBuilder().setValue("mom").setSpoken("Mom").build()))
                session.restoreTriggerState(session.triggerState())
            }
        }
        assertTrue(created.get() > 0 && created.get() == closed.get(), "created ${created.get()}, closed ${closed.get()}")
    }

    @Test
    fun aRuntimeWithAKotlinEncoderAndEntityModel() {
        Providers(builtIn = false).use { providers ->
            register(providers)
            val made = AtomicInteger()
            providers.sentenceEncoder("kotlin") { _, _ ->
                made.incrementAndGet()
                object : SentenceEncoderProvider { override fun encode(sentence: String) = floatArrayOf(sentence.length.toFloat(), 0f, 1f) }
            }
            providers.entityModel("kotlin") { _, _ ->
                made.incrementAndGet()
                object : EntityModelProvider {
                    override fun extract(words: List<String>, labels: List<EntityLabel>, sensitive: Boolean) = emptyList<EntitySpan>()
                }
            }
            val path = config("""providers { vad { id: "kotlin" } stt { id: "kotlin" } tts { id: "kotlin" } sentence_encoder { id: "kotlin" } entity_model { id: "kotlin" } }""")
            Runtime(path, providers, maxSessions = 2).use { runtime ->
                assertEquals(2, made.get())
                val first = runtime.session()
                val second = runtime.session()
                assertEquals(2, runtime.activeSessions())
                assertFailsWith<VasdkException> { runtime.session() }
                first.close()
                assertEquals(1, runtime.activeSessions())
                assertEquals(1, runtime.idleProviderSets())
                second.close()
            }
        }
    }

    private val agentText = """
        start_flow: "main"
        session_params { name: "tier" }
        entity_types { name: "contact" dynamic: true list { values { value: "nobody" } } }
        entity_types { name: "friend" code { extractor: "friends" } }
        intents { name: "call" examples: ["call @contact"] }
        intents { name: "ring" examples: ["ring @friend"] }
        flows { name: "main" start_page: "probe"
          pages { name: "probe" type: "probe"
                  routes { intent: "call" steps { say { text: "Calling ${'$'}nlu.entities.contact." } } target: "where" }
                  routes { intent: "ring" steps { say { text: "Ringing ${'$'}nlu.entities.friend." } } } }
          pages { name: "where" on_enter { say { text: "You're in ${'$'}context.city." } } }
        }
    """.trimIndent()

    @Test
    fun aTextConversationAndItsTurns() {
        val seen = mutableMapOf<String, String>()
        Registry().use { registry ->
            registry.pageType("probe", onEnter = { _, turn ->
                seen["flow"] = turn.flow
                seen["page"] = turn.page
                seen["rendered"] = turn.render("Tier \$session.tier")
                turn.say("Welcome.")
                turn.ask("Who should I call?", choices = listOf("Mom", "Dad"))
                seen["soFar"] = turn.replyText
            }, onTurn = { _, turn ->
                if (turn.text == "phone someone") turn.setText("call mom")
                false
            })
            registry.entityExtractor("friends") { words, _ ->
                val at = words.indexOf("bob")
                JsonArray(if (at < 0) emptyList() else listOf(jsonOf(mapOf("start" to at, "length" to 1, "value" to "bob", "spoken" to "Bob"))))
            }
            registry.addLanguage(Vasdk.builtinLanguage().toBuilder().setCode("en-GB").build())
            Agent.parse(agentText, registry).use { agent ->
                agent.conversation().use { conversation ->
                    conversation.setParam("tier", "gold")
                    conversation.setEntityValues("contact", listOf(EntityValue.newBuilder().setValue("mom").setSpoken("Mom").addSynonyms("mom").build()))
                    assertFailsWith<VasdkException> { conversation.setEntityValues("nothing", emptyList()) }
                    conversation.setContext(JsonObject(mapOf("city" to JsonPrimitive("Paris"))))

                    var reply = conversation.start()
                    assertTrue("Welcome." in reply.text && "Who should I call?" in reply.text && reply.expectsReply, reply.text)
                    assertEquals(listOf("Mom", "Dad"), reply.choicesList.map { it.label })
                    assertEquals(mapOf("flow" to "main", "page" to "probe", "rendered" to "Tier gold"), seen - "soFar")
                    assertTrue("Who should I call?" in seen["soFar"]!!)

                    reply = conversation.say("ring bob")
                    assertTrue("Ringing Bob." in reply.text, reply.text)

                    reply = conversation.say("phone someone")
                    assertTrue("Calling Mom." in reply.text && "You're in Paris." in reply.text && reply.page == "where", reply.text)

                    val snapshot = conversation.snapshot()
                    assertEquals("where", snapshot.page)
                    agent.conversation().use { again ->
                        val (resumed, fitted) = again.resume(snapshot)
                        assertTrue(fitted && resumed.page == "where")
                    }

                    conversation.replyInterrupted()
                    assertTrue(conversation.noInputMs() >= 0)
                    assertNull(conversation.nextScheduledTrigger())
                    assertTrue(conversation.restoreTriggerState(conversation.triggerState()))
                    assertEquals("", conversation.checkTriggers().text)
                    assertEquals("", conversation.deliverQueued().text)
                }

                fun case(contains: String) = FlowTestCase.newBuilder()
                    .addTurns(TestTurn.newBuilder().setExpect(TurnExpectation.newBuilder().addSaidContains(contains))).build()
                assertTrue(Vasdk.runTest(agent, case("Welcome.")).first)
                val (passed, report) = Vasdk.runTest(agent, case("Goodbye."))
                assertTrue(!passed && report.isNotEmpty())
            }
        }
    }

    @Test
    fun theLibraryAnswers() {
        assertTrue(Vasdk.version().isNotEmpty())
        assertFailsWith<VasdkException> { Vasdk.setLogLevel("loud") }
        assertTrue(File(Lib.path).exists() || Lib.path == "VASDK")
    }
}
