package com.vasdk.android.tools

import android.Manifest
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.CallLog
import android.provider.CalendarContract
import android.provider.ContactsContract
import android.telephony.SmsManager
import androidx.core.content.ContextCompat
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeParseException
import java.util.Date
import java.util.Locale

typealias Phone = ContactsContract.CommonDataKinds.Phone
object CommunicationTools {

    val toolNames = setOf(
        "get_contacts",
        "get_call_logs",
        "make_phone_call",
        "get_calendar_events",
        "add_calendar_event",
        "send_sms"
    )

    fun owns(toolName: String) = toolName in toolNames

    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "get_contacts"        -> getContacts(context, argsJson)
        "get_call_logs"       -> getCallLogs(context, argsJson)
        "make_phone_call"     -> makePhoneCall(context, argsJson)
        "get_calendar_events" -> getCalendarEvents(context, argsJson)
        "add_calendar_event"  -> addCalendarEvent(context, argsJson)
        "send_sms"            -> sendSms(context, argsJson)
        else                  -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    private fun hasPermission(context: Context, permission: String) =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    //  Contact resolution 

    private data class ContactMatch(val name: String, val number: String, val label: String)

    private sealed class ResolveResult {
        data class Resolved(val number: String, val name: String? = null) : ResolveResult()
        data class Ambiguous(val matches: List<ContactMatch>)             : ResolveResult()
        data class NotFound(val query: String)                            : ResolveResult()
    }

    // Matches strings that are already phone numbers (digits, +, -, spaces, parens)
    private val phoneNumberRegex = Regex("^[+\\d][\\d\\s().+\\-]*$")

    private fun phoneTypeLabel(type: Int): String = when (type) {
        Phone.TYPE_MOBILE      -> "mobile"
        Phone.TYPE_HOME        -> "home"
        Phone.TYPE_WORK        -> "work"
        Phone.TYPE_MAIN        -> "main"
        Phone.TYPE_WORK_MOBILE -> "work_mobile"
        Phone.TYPE_FAX_HOME    -> "fax_home"
        Phone.TYPE_FAX_WORK    -> "fax_work"
        else -> "other"
    }

    // Strip punctuation and collapse whitespace so "Dr. Smith" == "dr smith"
    private fun normalizeName(s: String): String =
        s.lowercase()
            .replace(Regex("[^a-z0-9 ]"), " ")
            .replace(Regex(" +"), " ")
            .trim()

    // Jaro-Winkler similarity (0.0 – 1.0). Operates on pre-normalized strings.
    private fun jaroWinkler(a: String, b: String): Double {
        if (a == b) return 1.0
        if (a.isEmpty() || b.isEmpty()) return 0.0
        val matchDist = maxOf(a.length, b.length) / 2 - 1
        val aM = BooleanArray(a.length)
        val bM = BooleanArray(b.length)
        var matches = 0
        for (i in a.indices) {
            val lo = maxOf(0, i - matchDist)
            val hi = minOf(i + matchDist + 1, b.length)
            for (j in lo until hi) {
                if (bM[j] || a[i] != b[j]) continue
                aM[i] = true; bM[j] = true; matches++; break
            }
        }
        if (matches == 0) return 0.0
        var trans = 0; var k = 0
        for (i in a.indices) {
            if (!aM[i]) continue
            while (!bM[k]) k++
            if (a[i] != b[k]) trans++
            k++
        }
        val jaro = (matches.toDouble() / a.length +
                    matches.toDouble() / b.length +
                    (matches - trans / 2.0) / matches) / 3.0
        var prefix = 0
        for (i in 0 until minOf(4, minOf(a.length, b.length))) {
            if (a[i] == b[i]) prefix++ else break
        }
        return jaro + prefix * 0.1 * (1.0 - jaro)
    }

    // Score the input against a contact name.
    // Checks the full name AND each individual token so "Smith" matches "John Smith".
    private fun matchScore(normInput: String, contactName: String): Double {
        val normName = normalizeName(contactName)
        val fullScore = jaroWinkler(normInput, normName)
        val tokenScore = normName.split(' ').filter { it.isNotEmpty() }
            .maxOfOrNull { jaroWinkler(normInput, it) } ?: 0.0
        return maxOf(fullScore, tokenScore)
    }

    private fun resolveContact(context: Context, input: String): ResolveResult {
        val trimmed = input.trim()

        // Already a phone number - pass through without a contacts query
        if (phoneNumberRegex.matches(trimmed)) return ResolveResult.Resolved(trimmed)

        if (!hasPermission(context, Manifest.permission.READ_CONTACTS))
            return ResolveResult.NotFound(
                "$trimmed (contacts cannot be searched: the user has not granted Contacts " +
                "access - they can enable it for this app in Android settings)"
            )

        val projection = arrayOf(Phone.DISPLAY_NAME, Phone.NUMBER, Phone.TYPE)
        val grouped    = LinkedHashMap<String, MutableList<ContactMatch>>()

        context.contentResolver.query(
            Phone.CONTENT_URI, projection, null, null, "${Phone.DISPLAY_NAME} ASC"
        )?.use { c ->
            val iName   = c.getColumnIndex(Phone.DISPLAY_NAME)
            val iNumber = c.getColumnIndex(Phone.NUMBER)
            val iType   = c.getColumnIndex(Phone.TYPE)
            while (c.moveToNext()) {
                val name   = c.getString(iName)   ?: continue
                val number = c.getString(iNumber) ?: continue
                grouped.getOrPut(name) { mutableListOf() }
                    .add(ContactMatch(name, number, phoneTypeLabel(c.getInt(iType))))
            }
        }

        val normInput = normalizeName(trimmed)
        // Keep only contacts whose best-token score clears the threshold
        val hits = grouped.entries
            .map    { (name, phones) -> Pair(matchScore(normInput, name), phones) }
            .filter { (score, _)     -> score >= FUZZY_THRESHOLD }
            .sortedByDescending { (score, _) -> score }

        if (hits.isEmpty()) return ResolveResult.NotFound(trimmed)

        // Contacts within 10% of the top score are considered tied
        val topScore = hits[0].first
        val topTier  = hits.takeWhile { (score, _) -> score >= topScore - 0.10 }

        if (topTier.size == 1) {
            val phones = topTier[0].second
            val name   = phones.firstOrNull()?.name
            val deduped = phones.distinctBy { it.number.filter(Char::isDigit) }
            return when {
                deduped.size == 1 -> ResolveResult.Resolved(deduped[0].number, name)
                deduped.count { it.label == "mobile" } == 1 ->
                    ResolveResult.Resolved(deduped.first { it.label == "mobile" }.number, name)
                else -> ResolveResult.Ambiguous(deduped)
            }
        }
        return ResolveResult.Ambiguous(topTier.flatMap { it.second }
            .distinctBy { it.number.filter(Char::isDigit) })
    }

    private const val FUZZY_THRESHOLD = 0.75

    private fun ambiguousJson(matches: List<ContactMatch>): String =
        JSONObject().apply {
            put("error", "Multiple contacts found - please specify a name or use the number directly")
            put("matches", JSONArray().also { arr ->
                matches.forEach { m ->
                    arr.put(JSONObject().apply {
                        put("name",   m.name)
                        put("number", m.number)
                        put("label",  m.label)
                    })
                }
            })
        }.toString()

    // Tracks the action actually offered for confirmation so a `confirmed=true` call can be
    // verified against it - the LLM's own `confirmed` flag can't be trusted on its own since a
    // small on-device model may emit it straight away without ever asking the user.
    private data class PendingAction(val tool: String, val number: String, val message: String?)
    @Volatile private var pendingAction: PendingAction? = null

    private fun requestConfirmation(tool: String, name: String?, number: String, message: String? = null): String {
        pendingAction = PendingAction(tool, number, message)
        ConfirmationSignal.notifyAwaiting(true)
        return confirmationRequiredJson(tool, name, number, message)
    }

    // A confirmed=true call only proceeds if it matches the action that was actually offered
    // for confirmation - otherwise it's re-routed back into a fresh confirmation request.
    private fun isConfirmedMatch(tool: String, number: String, message: String? = null): Boolean {
        val pending = pendingAction
        return pending != null && pending.tool == tool && pending.number == number && pending.message == message
    }

    private fun confirmationRequiredJson(action: String, name: String?, number: String, message: String? = null): String =
        JSONObject().apply {
            put("requires_confirmation", true)
            put("action", action)
            if (name != null) put("contact_name", name)
            put("number", number)
            if (message != null) put("message", message)
            put(
                "instruction",
                "Do not $action yet. Read the " +
                    (if (message != null) "recipient and message content" else "recipient") +
                    " back to the user out loud and ask them to confirm. Only call this tool again " +
                    "with confirmed=true after the user has verbally agreed."
            )
        }.toString()

    //  Contacts 

    private fun getContacts(context: Context, argsJson: String): String {
        if (!hasPermission(context, Manifest.permission.READ_CONTACTS))
            return JSONObject().put(
                "error",
                "Cannot read contacts: the user has not granted Contacts access. " +
                "Tell the user they can enable the Contacts permission for this app in Android settings."
            ).toString()

        val args  = JSONObject(argsJson)
        val query = args.optString("query", "").trim()
        if (query.isEmpty())
            return JSONObject().put("error", "A name or phone number to search for is required").toString()

        val projection = arrayOf(Phone.DISPLAY_NAME, Phone.NUMBER, Phone.TYPE)

        val grouped = LinkedHashMap<String, MutableList<JSONObject>>()
        context.contentResolver.query(
            Phone.CONTENT_URI,
            projection, null, null,
            "${Phone.DISPLAY_NAME} ASC"
        )?.use { c ->
            val iName   = c.getColumnIndex(Phone.DISPLAY_NAME)
            val iNumber = c.getColumnIndex(Phone.NUMBER)
            val iType   = c.getColumnIndex(Phone.TYPE)
            while (c.moveToNext()) {
                val name   = c.getString(iName)   ?: continue
                val number = c.getString(iNumber) ?: continue
                grouped.getOrPut(name) { mutableListOf() }.add(
                    JSONObject().apply {
                        put("number", number)
                        put("label",  phoneTypeLabel(c.getInt(iType)))
                    }
                )
            }
        }

        val names = if (phoneNumberRegex.matches(query)) {
            // Phone-number search: match contacts with a number containing these digits
            val queryDigits = query.filter { it.isDigit() }
            if (queryDigits.isEmpty()) emptyList() else grouped.entries
                .filter { (_, phones) ->
                    phones.any { it.getString("number").filter { d -> d.isDigit() }.contains(queryDigits) }
                }
                .map { it.key }
        } else {
            // Name search: rank by fuzzy similarity instead of an exact substring match, so
            // typos/partial names (e.g. "jon" or "smth") still surface the right contact.
            val normInput = normalizeName(query)
            grouped.keys
                .map { name -> Pair(matchScore(normInput, name), name) }
                .filter { (score, _) -> score >= FUZZY_THRESHOLD }
                .sortedByDescending { (score, _) -> score }
                .map { (_, name) -> name }
        }

        val results = JSONArray()
        for (name in names) {
            if (results.length() >= 50) break
            results.put(JSONObject().apply {
                put("name",   name)
                put("phones", JSONArray(grouped.getValue(name)))
            })
        }
        return JSONObject().put("contacts", results).toString()
    }

    //  Call logs 

    private fun getCallLogs(context: Context, argsJson: String): String {
        if (!hasPermission(context, Manifest.permission.READ_CALL_LOG))
            return JSONObject().put(
                "error",
                "Cannot read the call history: the user has not granted Call Log access. " +
                "Tell the user they can enable the Call Log permission for this app in Android settings."
            ).toString()

        val args  = JSONObject(argsJson)
        val limit = args.optInt("limit", 10).coerceIn(1, 100)

        val projection = arrayOf(
            CallLog.Calls.NUMBER,
            CallLog.Calls.CACHED_NAME,
            CallLog.Calls.TYPE,
            CallLog.Calls.DURATION,
            CallLog.Calls.DATE
        )
        val callTypes = mapOf(
            CallLog.Calls.INCOMING_TYPE to "incoming",
            CallLog.Calls.OUTGOING_TYPE to "outgoing",
            CallLog.Calls.MISSED_TYPE   to "missed",
            CallLog.Calls.REJECTED_TYPE to "rejected"
        )
        val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())

        val results = JSONArray()
        context.contentResolver.query(
            CallLog.Calls.CONTENT_URI,
            projection, null, null,
            "${CallLog.Calls.DATE} DESC"
        )?.use { c ->
            val iNum  = c.getColumnIndex(CallLog.Calls.NUMBER)
            val iName = c.getColumnIndex(CallLog.Calls.CACHED_NAME)
            val iType = c.getColumnIndex(CallLog.Calls.TYPE)
            val iDur  = c.getColumnIndex(CallLog.Calls.DURATION)
            val iDate = c.getColumnIndex(CallLog.Calls.DATE)
            while (c.moveToNext() && results.length() < limit) {
                val number = c.getString(iNum) ?: ""
                val name   = c.getString(iName)?.trim()?.takeIf { it.isNotEmpty() } ?: "unknown"
                results.put(JSONObject().apply {
                    put("name", name)
                    // Only surface the number when there's no cached name - otherwise a small
                    // on-device model tends to read back whichever field looks more like "the
                    // answer" and picks the number even when told to prefer the name.
                    if (name == "unknown") put("number", number)
                    put("type",         callTypes[c.getInt(iType)] ?: "unknown")
                    put("duration_sec", c.getLong(iDur))
                    put("date",         fmt.format(Date(c.getLong(iDate))))
                })
            }
        }
        return JSONObject().put("call_logs", results).toString()
    }

    //  Phone call 

    private fun makePhoneCall(context: Context, argsJson: String): String {
        if (!hasPermission(context, Manifest.permission.CALL_PHONE))
            return JSONObject().put(
                "error",
                "Cannot place calls: the user has not granted Phone access. " +
                "Tell the user they can enable the Phone permission for this app in Android settings."
            ).toString()

        val args      = JSONObject(argsJson)
        val contact   = args.getString("contact")
        val confirmed = args.optBoolean("confirmed", false)

        return when (val r = resolveContact(context, contact)) {
            is ResolveResult.Resolved  -> {
                if (!confirmed || !isConfirmedMatch("make_phone_call", r.number)) {
                    requestConfirmation("make_phone_call", r.name, r.number)
                } else {
                    pendingAction = null
                    ConfirmationSignal.notifyAwaiting(false)
                    val safeNumber = r.number.filter { it.isDigit() || it == '+' }
                    IntentDispatch.fire(context, Intent(Intent.ACTION_CALL, Uri.parse("tel:$safeNumber")), "calling $safeNumber")
                }
            }
            is ResolveResult.Ambiguous -> ambiguousJson(r.matches)
            is ResolveResult.NotFound  -> JSONObject().put("error", "No contact found matching: ${r.query}").toString()
        }
    }

    //  Calendar events 

    private fun getCalendarEvents(context: Context, argsJson: String): String {
        if (!hasPermission(context, Manifest.permission.READ_CALENDAR))
            return JSONObject().put(
                "error",
                "Cannot read the calendar: the user has not granted Calendar access. " +
                "Tell the user they can enable the Calendar permission for this app in Android settings."
            ).toString()

        val args      = JSONObject(argsJson)
        val daysAhead = args.optInt("days_ahead", 7).coerceIn(1, 90)
        val now       = System.currentTimeMillis()
        val end       = now + daysAhead * 86_400_000L

        val projection = arrayOf(
            CalendarContract.Events.TITLE,
            CalendarContract.Events.DTSTART,
            CalendarContract.Events.DTEND,
            CalendarContract.Events.EVENT_LOCATION,
            CalendarContract.Events.DESCRIPTION
        )
        val selection = "${CalendarContract.Events.DTSTART} >= ? AND ${CalendarContract.Events.DTSTART} <= ?"
        val fmt       = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())

        val results = JSONArray()
        context.contentResolver.query(
            CalendarContract.Events.CONTENT_URI,
            projection, selection, arrayOf(now.toString(), end.toString()),
            "${CalendarContract.Events.DTSTART} ASC"
        )?.use { c ->
            val iTitle    = c.getColumnIndex(CalendarContract.Events.TITLE)
            val iStart    = c.getColumnIndex(CalendarContract.Events.DTSTART)
            val iEnd      = c.getColumnIndex(CalendarContract.Events.DTEND)
            val iLocation = c.getColumnIndex(CalendarContract.Events.EVENT_LOCATION)
            val iDesc     = c.getColumnIndex(CalendarContract.Events.DESCRIPTION)
            while (c.moveToNext() && results.length() < 50) {
                results.put(JSONObject().apply {
                    put("title",       c.getString(iTitle)    ?: "")
                    put("start",       fmt.format(Date(c.getLong(iStart))))
                    put("end",         fmt.format(Date(c.getLong(iEnd))))
                    put("location",    c.getString(iLocation) ?: "")
                    put("description", c.getString(iDesc)     ?: "")
                })
            }
        }
        return JSONObject().put("events", results).toString()
    }

    //  Add calendar event

    // The tool description tells the model to pass a plain ISO 8601 local time
    // (e.g. "2025-06-01T14:00:00", no offset), which Instant.parse rejects. Accept
    // both that form and a proper offset/instant, falling back to system default zone.
    private fun parseEventTime(value: String): Long =
        try {
            Instant.parse(value).toEpochMilli()
        } catch (e: DateTimeParseException) {
            LocalDateTime.parse(value).atZone(ZoneId.systemDefault()).toInstant().toEpochMilli()
        }

    private fun addCalendarEvent(context: Context, argsJson: String): String {
        if (!hasPermission(context, Manifest.permission.WRITE_CALENDAR))
            return JSONObject().put(
                "error",
                "Cannot add calendar events: the user has not granted Calendar access. " +
                "Tell the user they can enable the Calendar permission for this app in Android settings."
            ).toString()

        val args        = JSONObject(argsJson)
        val description = args.optString("description", "")
        // title is the required short event name; fall back to description if the
        // model omitted it, since it's the field most often confused with title.
        val title       = args.optString("title", "").ifBlank { description }.ifBlank {
            return JSONObject().put(
                "error",
                "Missing required parameter: title. Provide a short event title, e.g. \"Dentist appointment\"."
            ).toString()
        }
        val hasStartTime = args.has("start_time") && !args.isNull("start_time")
        val allDay: Boolean
        val startMs: Long
        val endMs: Long
        if (hasStartTime) {
            allDay = false
            startMs = try {
                parseEventTime(args.getString("start_time"))
            } catch (e: DateTimeParseException) {
                return JSONObject().put("error", "Invalid start_time, expected ISO 8601 (e.g. 2025-06-01T14:00:00)").toString()
            }
            endMs = if (args.has("end_time") && !args.isNull("end_time")) {
                try {
                    parseEventTime(args.getString("end_time"))
                } catch (e: DateTimeParseException) {
                    return JSONObject().put("error", "Invalid end_time, expected ISO 8601 (e.g. 2025-06-01T15:00:00)").toString()
                }
            } else {
                startMs + 60 * 60 * 1000L
            }
        } else {
            // No start_time given: treat as an all-day event for today. Android
            // all-day events are stored as UTC midnight boundaries regardless of
            // the device's local timezone.
            allDay = true
            val today = LocalDate.now()
            startMs = today.atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()
            endMs = today.plusDays(1).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()
        }
        val location    = args.optString("location", "")

        val calId = context.contentResolver.query(
            CalendarContract.Calendars.CONTENT_URI,
            arrayOf(CalendarContract.Calendars._ID),
            "${CalendarContract.Calendars.VISIBLE} = 1",
            null, null
        )?.use { it.moveToFirst(); if (it.count > 0) it.getLong(0) else null }
            ?: return JSONObject().put("error", "No calendar found on device").toString()

        val values = ContentValues().apply {
            put(CalendarContract.Events.CALENDAR_ID,    calId)
            put(CalendarContract.Events.TITLE,           title)
            put(CalendarContract.Events.DTSTART,         startMs)
            put(CalendarContract.Events.DTEND,           endMs)
            put(CalendarContract.Events.DESCRIPTION,     description)
            put(CalendarContract.Events.EVENT_LOCATION,  location)
            put(CalendarContract.Events.ALL_DAY,         if (allDay) 1 else 0)
            put(CalendarContract.Events.EVENT_TIMEZONE,  if (allDay) "UTC" else java.util.TimeZone.getDefault().id)
        }
        val uri = context.contentResolver.insert(CalendarContract.Events.CONTENT_URI, values)
        return if (uri != null) {
            JSONObject().apply {
                put("created",  true)
                put("event_id", uri.lastPathSegment)
                put("title",    title)
            }.toString()
        } else {
            JSONObject().put("error", "Failed to insert calendar event").toString()
        }
    }

    //  Send SMS 

    private fun sendSms(context: Context, argsJson: String): String {
        if (!hasPermission(context, Manifest.permission.SEND_SMS))
            return JSONObject().put(
                "error",
                "Cannot send text messages: the user has not granted SMS access. " +
                "Tell the user they can enable the SMS permission for this app in Android settings."
            ).toString()

        val args      = JSONObject(argsJson)
        val contact   = args.getString("contact")
        val message   = args.getString("message")
        val confirmed = args.optBoolean("confirmed", false)

        val resolved = when (val r = resolveContact(context, contact)) {
            is ResolveResult.Resolved  -> r
            is ResolveResult.Ambiguous -> return ambiguousJson(r.matches)
            is ResolveResult.NotFound  -> return JSONObject().put("error", "No contact found matching: ${r.query}").toString()
        }

        if (!confirmed || !isConfirmedMatch("send_sms", resolved.number, message)) {
            return requestConfirmation("send_sms", resolved.name, resolved.number, message)
        }
        pendingAction = null
        ConfirmationSignal.notifyAwaiting(false)

        val number = resolved.number.filter { it.isDigit() || it == '+' }
        return try {
            @Suppress("DEPRECATION")
            val sm = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S)
                context.getSystemService(SmsManager::class.java)
            else
                SmsManager.getDefault()

            sm.sendTextMessage(number, null, message, null, null)
            JSONObject().apply {
                put("sent", true)
                put("to",   number)
            }.toString()
        } catch (e: Exception) {
            JSONObject().put("error", e.message ?: "SMS send failed").toString()
        }
    }
}
