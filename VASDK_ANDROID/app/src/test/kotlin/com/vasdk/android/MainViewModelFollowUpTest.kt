package com.vasdk.android

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test

/**
 * MainViewModel through a merged turn, as the SDK's events reach it: the reply begun to the first
 * part is retracted when the user goes on, the answer to the merged turn streams into its place,
 * a retracted reply no answer comes for goes, and a false start takes the speaking bubble away.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class MainViewModelFollowUpTest {
    private val dispatcher = StandardTestDispatcher()
    private lateinit var viewModel: MainViewModel

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
        viewModel = MainViewModel()
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private fun shown() = viewModel.uiState.value.messages.map { message ->
        when (message.role) {
            MainMessageRole.USER -> "user: ${message.text}" + (if (message.mergedParts > 1) " (${message.mergedParts} parts)" else "")
            MainMessageRole.ASSISTANT -> "assistant: ${message.text}" + (if (message.isRetracted) " (retracted)" else "")
            MainMessageRole.THINKING -> "thinking"
            MainMessageRole.USER_SPEAKING -> "speaking"
            MainMessageRole.ERROR -> "error: ${message.text}"
        }
    }

    // "Book a flight", a reply begins, the user goes on "to Paris", and the SDK merges them.
    private fun resumeOverAReply() {
        viewModel.onTranscriptReceived("Book a flight", isFinal = true)
        viewModel.onLlmResponse("Sure, where to?", isFinal = false)
        viewModel.onTranscriptReceived("to Paris", isFinal = true)
        viewModel.onTranscriptReceived("Book a flight to Paris", isFinal = true, isFollowUp = true, replacesTranscripts = 2)
    }

    @Test
    fun theReplyToTheFirstPartIsRetracted() {
        resumeOverAReply()
        assertEquals(
            listOf("user: Book a flight to Paris (2 parts)", "assistant: Sure, where to? (retracted)"),
            shown()
        )
    }

    @Test
    fun theNewAnswerTakesThePlaceOfTheRetractedReply() {
        resumeOverAReply()
        val placeholder = viewModel.uiState.value.messages[1].id
        viewModel.onLlmResponse("A flight to Paris. ", isFinal = false)
        viewModel.onLlmResponse("From where?", isFinal = true)
        assertEquals(listOf("user: Book a flight to Paris (2 parts)", "assistant: A flight to Paris. From where?"), shown())
        assertEquals("the same message, so it morphs in place", placeholder, viewModel.uiState.value.messages[1].id)
    }

    @Test
    fun aRetractedReplyWithNoAnswerGoesWhenTheUserSpeaksAgain() {
        resumeOverAReply()
        viewModel.onTranscriptReceived("Never mind", isFinal = true)
        dispatcher.scheduler.advanceTimeBy(1_000)
        dispatcher.scheduler.runCurrent()
        assertEquals(listOf("user: Book a flight to Paris (2 parts)", "user: Never mind", "thinking"), shown())
    }

    @Test
    fun aFalseStartTakesTheSpeakingBubbleAway() {
        viewModel.onStatusChanged("SPEECH_STARTING")
        assertEquals(listOf("speaking"), shown())
        viewModel.onStatusChanged("SPEECH_CANCELLED")
        assertEquals(emptyList<String>(), shown())
    }
}
