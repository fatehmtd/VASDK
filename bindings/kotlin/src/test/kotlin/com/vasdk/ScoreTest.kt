// Transcript scores from Kotlin, through the C API: word and character edits by kind, the texts as
// scored, and a corpus rate from added counts. The port of bindings/python/tests/test_score.py.
package com.vasdk

import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class ScoreTest {
    @Test
    fun transcriptScores() {
        val score = Vasdk.scoreTranscript("The cat, sat on the mat.", "the cat sit on mat")
        assertEquals(EditCounts(1, 1, 0, 6, score.words.errorRate), score.words)
        assertTrue(abs(score.words.errorRate - 2.0 / 6.0) < 1e-9)
        assertEquals("the cat sat on the mat", score.reference)
        assertEquals("the cat sat on the mat".length.toLong(), score.characters.referenceLength)

        assertEquals("l'homme été déjà", Vasdk.normalizeTranscript("L’HOMME ÉTÉ, déjà!"))
        assertEquals(0.0, Vasdk.scoreTranscript("", "").words.errorRate)

        val clips = listOf("a" to "b", "a b c d e f g h i" to "a b c d e f g h i")
        val counted = clips.map { (said, heard) -> Vasdk.scoreTranscript(said, heard).words }
        assertEquals(0.1, counted.sumOf { it.errors }.toDouble() / counted.sumOf { it.referenceLength })
    }
}
