package com.vasdk.android.tools

import android.Manifest
import android.app.AlarmManager
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.content.pm.PackageInfoCompat
import android.content.pm.PackageManager
import android.provider.AlarmClock
import org.json.JSONObject
import java.util.Calendar
import java.util.concurrent.atomic.AtomicInteger

object NotificationTools {

    val toolNames = setOf("send_notification", "set_alarm_or_timer")

    fun owns(toolName: String) = toolName in toolNames

    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "send_notification"  -> sendNotification(context, argsJson)
        "set_alarm_or_timer" -> setAlarmOrTimer(context, argsJson)
        else                 -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    //  send_notification 

    private val notifIdCounter = AtomicInteger(1000)

    private fun sendNotification(context: Context, argsJson: String): String {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
                return JSONObject().put(
                    "error",
                    "Cannot show notifications: the user has not granted Notification access. " +
                    "Tell the user they can enable the Notifications permission for this app in Android settings."
                ).toString()
            }
        }
        val args = JSONObject(argsJson)
        val title     = args.optString("title", "Alert")
        val body      = args.optString("body", "")
        val channelId = args.optString("channel_id", "vasdk_alerts")

        val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(channelId, "VASDK Alerts", NotificationManager.IMPORTANCE_DEFAULT)
            )
        }

        val notif = NotificationCompat.Builder(context, channelId)
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle(title)
            .setContentText(body)
            .setAutoCancel(true)
            .build()

        val id = notifIdCounter.getAndIncrement()
        nm.notify(id, notif)
        return JSONObject().apply {
            put("sent", true)
            put("notification_id", id)
        }.toString()
    }

    //  set_alarm_or_timer
    //
    // The model previously chose between two separate tools (set_alarm/set_timer) and computed
    // hour/minute/seconds itself - on this small on-device model that reliably misfired (e.g.
    // "an alarm in five seconds" became hour=5, minute=0). Small models are far more reliable at
    // copying a phrase through than at classifying + doing arithmetic on it, so instead the model
    // just forwards the time expression close to verbatim in `when`, and classification/math
    // happens here deterministically.

    private fun setAlarmOrTimer(context: Context, argsJson: String): String {
        val args      = JSONObject(argsJson)
        val whenText  = args.getString("when")
        val label     = args.optString("label", "")

        parseDurationSeconds(whenText)?.let { seconds ->
            val intent = Intent(AlarmClock.ACTION_SET_TIMER).apply {
                putExtra(AlarmClock.EXTRA_LENGTH, seconds)
                if (label.isNotEmpty()) putExtra(AlarmClock.EXTRA_MESSAGE, label)
                // SKIP_UI=true: this is a voice-only flow with no one present to tap a
                // confirmation screen, so without this the clock app would show its own
                // "save?" UI and silently discard the timer once we move on.
                putExtra(AlarmClock.EXTRA_SKIP_UI, true)
            }
            return IntentDispatch.fire(context, intent, "timer set for ${seconds}s")
        }

        parseClockTime(whenText)?.let { (hour, minute) ->
            val intent = Intent(AlarmClock.ACTION_SET_ALARM).apply {
                putExtra(AlarmClock.EXTRA_HOUR, hour)
                putExtra(AlarmClock.EXTRA_MINUTES, minute)
                if (label.isNotEmpty()) putExtra(AlarmClock.EXTRA_MESSAGE, label)
                putExtra(AlarmClock.EXTRA_SKIP_UI, true)
            }
            return IntentDispatch.fire(context, intent, "alarm set for %02d:%02d".format(hour, minute))
        }

        return JSONObject().put(
            "error",
            "Could not understand \"$whenText\" as a time or duration. Ask the user to clarify " +
            "(e.g. \"7am\" for an alarm, or \"in 10 minutes\" for a timer)."
        ).toString()
    }

    private val NUMBER_WORDS = mapOf(
        "zero" to 0, "one" to 1, "two" to 2, "three" to 3, "four" to 4, "five" to 5,
        "six" to 6, "seven" to 7, "eight" to 8, "nine" to 9, "ten" to 10,
        "eleven" to 11, "twelve" to 12, "thirteen" to 13, "fourteen" to 14, "fifteen" to 15,
        "sixteen" to 16, "seventeen" to 17, "eighteen" to 18, "nineteen" to 19, "twenty" to 20,
        "thirty" to 30, "forty" to 40, "fifty" to 50
    )

    // Reads a number starting at `tokens[index]` (digits or spelled-out words, incl. compounds
    // like "twenty five"). Returns the value and how many tokens it consumed, or null.
    private fun readNumber(tokens: List<String>, index: Int): Pair<Int, Int>? {
        tokens.getOrNull(index)?.toIntOrNull()?.let { return it to 1 }
        val first = NUMBER_WORDS[tokens.getOrNull(index)] ?: return null
        if (first in listOf(20, 30, 40, 50)) {
            val next = NUMBER_WORDS[tokens.getOrNull(index + 1)]
            if (next != null && next < 10) return (first + next) to 2
        }
        return first to 1
    }

    private fun unitSeconds(unit: String): Int = when {
        unit.startsWith("h") -> 3600
        unit.startsWith("m") -> 60
        else                 -> 1
    }

    private val DURATION_UNIT = Regex("seconds?|secs?|minutes?|mins?|hours?|hrs?")

    private fun parseDurationSeconds(whenText: String): Int? {
        val tokens = whenText.lowercase().split(Regex("[^a-z0-9]+")).filter { it.isNotBlank() }
        for (i in tokens.indices) {
            if (!DURATION_UNIT.matches(tokens[i])) continue
            for (start in maxOf(0, i - 2) until i) {
                val (value, consumed) = readNumber(tokens, start) ?: continue
                if (start + consumed == i) return value * unitSeconds(tokens[i])
            }
        }
        return null
    }

    // Picks whichever of the am/pm readings of an ambiguous 12-hour value comes up soonest
    // from now, since the device intent needs a definite 24-hour value either way.
    private fun resolveAmbiguousHour(hour12: Int, minute: Int): Int {
        val now        = Calendar.getInstance()
        val nowMinutes = now.get(Calendar.HOUR_OF_DAY) * 60 + now.get(Calendar.MINUTE)
        val amHour     = hour12 % 12
        val pmHour     = amHour + 12
        val amDelta    = ((amHour * 60 + minute) - nowMinutes).mod(24 * 60)
        val pmDelta    = ((pmHour * 60 + minute) - nowMinutes).mod(24 * 60)
        return if (amDelta <= pmDelta) amHour else pmHour
    }

    private fun parseClockTime(whenText: String): Pair<Int, Int>? {
        val lower = whenText.lowercase().trim()

        Regex("(\\d{1,2})(?::(\\d{2}))?\\s*(am|pm)?").find(lower)?.let { m ->
            val hourRaw = m.groupValues[1].toIntOrNull() ?: return@let
            val minute  = m.groupValues[2].toIntOrNull() ?: 0
            val ampm    = m.groupValues[3].takeIf { it.isNotEmpty() }
            if (hourRaw !in 0..23 || minute !in 0..59) return@let
            val hour = when (ampm) {
                "am" -> hourRaw % 12
                "pm" -> (hourRaw % 12) + 12
                else -> if (hourRaw in 1..12) resolveAmbiguousHour(hourRaw, minute) else hourRaw
            }
            return hour.coerceIn(0, 23) to minute
        }

        val tokens    = lower.split(Regex("[^a-z0-9]+")).filter { it.isNotBlank() }
        val ampmIndex = tokens.indexOfFirst { it == "am" || it == "pm" }
        for (i in tokens.indices) {
            val (value, consumed) = readNumber(tokens, i) ?: continue
            if (value !in 0..23) continue
            val hour = when {
                ampmIndex == i + consumed && tokens[ampmIndex] == "am" -> value % 12
                ampmIndex == i + consumed && tokens[ampmIndex] == "pm" -> (value % 12) + 12
                else -> continue
            }
            return hour to 0
        }
        return null
    }
}
