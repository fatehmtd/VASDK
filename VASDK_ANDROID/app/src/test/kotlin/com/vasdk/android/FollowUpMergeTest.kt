package com.vasdk.android

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * mergeFollowUp: a follow-up transcript turns the user's messages it replaces into one, in the place
 * of the first, marks the assistant's cut-off reply between them, leaves back-channels and earlier
 * turns alone, and counts an earlier follow-up as the parts it holds.
 */
class FollowUpMergeTest {
    private var nextId = 1L

    private fun user(text: String, backchannel: Boolean = false, durationMs: Long = 0L) =
        MainMessage(id = nextId++, role = MainMessageRole.USER, text = text, isBackchannel = backchannel, speechDurationMs = durationMs)

    private fun assistant(text: String) = MainMessage(id = nextId++, role = MainMessageRole.ASSISTANT, text = text)

    private fun thinking() = MainMessage(id = nextId++, role = MainMessageRole.THINKING)

    // Each message as the chat shows it, e.g. "user: Book a flight to Paris (2 parts)"
    private fun shown(messages: List<MainMessage>) = messages.map { message ->
        when (message.role) {
            MainMessageRole.USER -> "user: ${message.text}" + (if (message.mergedParts > 1) " (${message.mergedParts} parts)" else "") +
                (if (message.isBackchannel) " (back-channel)" else "")
            MainMessageRole.ASSISTANT -> "assistant: ${message.text}" + (if (message.isInterrupted) " (interrupted)" else "")
            MainMessageRole.THINKING -> "thinking"
            else -> message.role.name
        }
    }

    @Test
    fun resumedWhileThinkingBecomesOneMessage() {
        val earlier = listOf(user("Hi"), assistant("Hello!"))
        val conversation = earlier + listOf(user("Book a flight", durationMs = 900), user("to Paris", durationMs = 600), thinking())
        val merge = mergeFollowUp(conversation, "Book a flight to Paris", 2, newId = 100, nowMs = 5)
        assertEquals(
            listOf("user: Hi", "assistant: Hello!", "user: Book a flight to Paris (2 parts)", "thinking"),
            shown(merge.messages)
        )
        assertEquals("the merged message keeps the first part's id", conversation[2].id, merge.mergedId)
        assertEquals(setOf(conversation[3].id), merge.removedIds)
        assertEquals("speech time adds up", 1500L, merge.messages[2].speechDurationMs)
        assertEquals("stamped for the highlight", 5L, merge.messages[2].mergedAtMs)
    }

    @Test
    fun replyCutOffByTheResumeIsMarkedInterrupted() {
        val conversation = listOf(user("Book a flight"), assistant("Sure, where"), user("to Paris"), thinking())
        val merge = mergeFollowUp(conversation, "Book a flight to Paris", 2, newId = 100, nowMs = 5)
        assertEquals(
            listOf("user: Book a flight to Paris (2 parts)", "assistant: Sure, where (interrupted)", "thinking"),
            shown(merge.messages)
        )
        assertEquals(setOf(conversation[1].id), merge.voidIds)
    }

    @Test
    fun backChannelIsLeftAlone() {
        val conversation = listOf(user("Book a flight"), user("mm-hmm", backchannel = true), user("to Paris"))
        val merge = mergeFollowUp(conversation, "Book a flight to Paris", 2, newId = 100, nowMs = 5)
        assertEquals(
            listOf("user: Book a flight to Paris (2 parts)", "user: mm-hmm (back-channel)"),
            shown(merge.messages)
        )
    }

    @Test
    fun secondFollowUpCountsTheFirstAsItsParts() {
        val first = mergeFollowUp(listOf(user("Yeah."), user("Do you think")), "Yeah. Do you think", 2, newId = 100, nowMs = 5)
        val second = mergeFollowUp(
            first.messages + user("I have a contact named Heba?"),
            "Yeah. Do you think I have a contact named Heba?", 2, newId = 101, nowMs = 9
        )
        assertEquals(listOf("user: Yeah. Do you think I have a contact named Heba? (3 parts)"), shown(second.messages))
    }

    @Test
    fun nothingToMergeAddsTheMessage() {
        val merge = mergeFollowUp(listOf(assistant("Hello!")), "Book a flight to Paris", 2, newId = 100, nowMs = 5)
        assertEquals(listOf("assistant: Hello!", "user: Book a flight to Paris (2 parts)"), shown(merge.messages))
        assertEquals(100L, merge.mergedId)
    }
}
