package com.vasdk.android

/** The conversation after a follow-up transcript, and what changed in it. */
internal data class FollowUpMerge(
    val messages: List<MainMessage>,
    /** The merged user message (it keeps the id of the first part). */
    val mergedId: Long,
    /** Ids no longer in the conversation (the later parts). */
    val removedIds: Set<Long>,
    /** The assistant's replies between the parts: cut off and void, now marked interrupted. */
    val voidIds: Set<Long>
)

/**
 * A follow-up transcript (vasdk.engine.Transcript.is_follow_up): the user picked up again after a
 * pause that ended their turn, and the SDK merged their words into one turn. Its last
 * [replacesTranscripts] user messages (back-channels aside) become one message with [text], in
 * the place of the first; a reply the assistant began between them is marked interrupted.
 * Without any user message to merge, [text] is added as a new one with id [newId]. [nowMs] stamps
 * the merged message (MainMessage.mergedAtMs).
 */
internal fun mergeFollowUp(
    messages: List<MainMessage>,
    text: String,
    replacesTranscripts: Int,
    newId: Long,
    nowMs: Long
): FollowUpMerge {
    val parts = messages.indices
        .filter { messages[it].role == MainMessageRole.USER && !messages[it].isBackchannel }
        .takeLast(maxOf(1, replacesTranscripts))
    if (parts.isEmpty()) {
        val added = MainMessage(
            id = newId, role = MainMessageRole.USER, text = text,
            mergedParts = maxOf(2, replacesTranscripts), mergedAtMs = nowMs
        )
        return FollowUpMerge(messages + added, newId, emptySet(), emptySet())
    }
    val first = parts.first()
    val partSet = parts.toSet()
    // An earlier follow-up counts as the parts it already holds
    val totalParts = parts.sumOf { maxOf(1, messages[it].mergedParts) }
    val merged = messages[first].copy(
        text = text,
        isPartial = false,
        mergedParts = maxOf(2, totalParts),
        mergedAtMs = nowMs,
        speechDurationMs = parts.sumOf { messages[it].speechDurationMs }
    )
    val removed = mutableSetOf<Long>()
    val void = mutableSetOf<Long>()
    val result = messages.mapIndexedNotNull { index, message ->
        when {
            index == first -> merged
            index in partSet -> { removed += message.id; null }
            index > first && message.role == MainMessageRole.ASSISTANT -> {
                void += message.id
                message.copy(isInterrupted = true)
            }
            else -> message
        }
    }
    return FollowUpMerge(result, merged.id, removed, void)
}
