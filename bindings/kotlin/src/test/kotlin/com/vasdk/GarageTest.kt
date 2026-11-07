// The garage agent's whole suite from Kotlin: its C++ step, envelope and tool written again in
// Kotlin through the C API, and every test case played through them; then a booking by hand.
// The port of bindings/python/tests/test_garage.py.
package com.vasdk

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonPrimitive
import vasdk.flow.StepOutcomeOuterClass.StepOutcome
import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

class GarageTest {
    private val folder = File(System.getenv("VASDK_GARAGE_DIR") ?: "../../VASDK_APP/agents/garage-menu")

    /** What extensions/GarageExtensions.cpp registers, in Kotlin. */
    private fun garageExtensions(bookings: MutableList<JsonObject>): Registry {
        val registry = Registry()
        // garage.BookingReference: its .proto is the agent's own, so its params come as JSON
        registry.stepJson("garage.BookingReference") { params, turn ->
            // "AG-" and four digits from the booking's values (FNV-1a), as the C++ step makes it
            val booking = listOf("\$form.service", "\$form.day", "\$form.time", "\$form.phone").joinToString("") { slot ->
                val value = turn.lookup(slot)
                (if (value is JsonPrimitive) value.content else "") + "|"
            }
            var hash = 2166136261L
            booking.toByteArray(Charsets.UTF_8).forEach { byte -> hash = ((hash xor (byte.toLong() and 0xff)) * 16777619L) and 0xFFFFFFFFL }
            val into = (params as JsonObject)["into"]!!.jsonPrimitive.content
            turn.apply(StepOutcome.newBuilder().putSet(into, jsonOf("AG-${1000 + hash % 9000}").toValue()).build())
        }
        registry.tool("save_booking", "Keeps a confirmed booking") { booking ->
            val reference = (booking["reference"] as? JsonPrimitive)?.content
            require(!reference.isNullOrEmpty()) { "a booking needs a reference" }
            synchronized(bookings) { bookings += booking }
            jsonOf(mapOf("reference" to reference, "status" to "confirmed"))
        }
        val thanksWords = setOf("thanks", "thank", "you", "very", "much", "cheers", "ok", "okay")
        registry.envelope("thanks", before = { turn ->
            val said = Regex("[a-z0-9']+").findAll(turn.text.lowercase()).map { it.value }.toList()
            when {
                said.isEmpty() || said.any { it !in thanksWords } -> false   // more than thanks: "no that's all thanks"
                said.any { it in setOf("thanks", "thank", "cheers") } -> { turn.say("You're welcome."); true }
                else -> false
            }
        })
        return registry
    }

    @Test
    fun theWholeSuiteAndABookingByHand() {
        Vasdk.setLogLevel("warn")
        val bookings = mutableListOf<JsonObject>()
        garageExtensions(bookings).use { registry ->
            Agent.load(File(folder, "agent.txtpb").path, registry).use { agent ->
                val cases = File(folder, "tests").listFiles { file -> file.name.endsWith(".txtpb") }!!.sorted()
                val failed = cases.filter { case ->
                    val (passed, report) = Vasdk.runTestFile(agent, case.path)
                    println((if (passed) "PASS " else "FAIL ") + case.name + if (passed) "" else "\n    " + report.trim().replace("\n", "\n    "))
                    !passed
                }
                assertTrue(cases.isNotEmpty(), "no test cases in $folder")
                assertEquals(emptyList(), failed.map { it.name }, "test cases that failed")

                agent.conversation().use { conversation ->
                    assertTrue("Acme Garage" in conversation.start().text)
                    assertEquals("booking", conversation.say("book a tire repair on tuesday").page)
                    conversation.say("2 pm")
                    conversation.say("555 123 4567")
                    val reply = conversation.say("yes")
                    assertEquals("booked", reply.page, reply.text)
                    assertTrue("AG-" in reply.text, reply.text)
                    assertTrue(reply.toolCallsList.any { it.name == "save_booking" && it.ok }, "save_booking was called")
                    assertTrue(conversation.lookup("\$session.reference").jsonPrimitive.content.startsWith("AG-"))
                    assertTrue(bookings.isNotEmpty())
                    assertFailsWith<VasdkException> { conversation.setParam("nope", 1) }
                }
            }
        }
        val broken = assertFailsWith<VasdkException> { Agent.parse("flows {") }
        assertTrue(broken.message!!.isNotEmpty())
    }
}
