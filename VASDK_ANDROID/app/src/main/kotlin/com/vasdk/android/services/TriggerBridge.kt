package com.vasdk.android.services

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.service.notification.StatusBarNotification
import android.util.Log
import com.vasdk.android.MainActivity
import com.vasdk.android.VasdkEngine
import org.json.JSONObject

/**
 * The agent's triggers while the app isn't in front.
 *
 * **Schedule triggers.** While the engine runs it checks them each second. The triggers' state
 * and the next due time are kept here (each minute and on stop, in shared preferences), with an
 * AlarmManager alarm at that time. When the alarm rings and the engine is running, its own check
 * says the reminder. When it isn't, a notification asks the user to open the app (Android 14+
 * doesn't let an app in the background start the microphone); started again, the engine restores
 * the state, and the reminder that came due is said at its first check, once.
 *
 * **Notifications.** With notification access granted, [VasdkNotificationListener] hands each
 * notification another app posts (not ongoing ones) to [onNotification], which fires the agent's
 * `notification` app event with `{"package", "title", "text", "category"}` as `$trigger.payload`:
 *
 *     triggers { name: "message" source { app_event { name: "notification" } }
 *                condition: "$trigger.payload.package == 'com.whatsapp'"
 *                action { say { text: "A message from $trigger.payload.title." } } }
 */
object TriggerBridge {
    private const val TAG = "VASDK.Triggers"
    private const val PREFERENCES = "vasdk_triggers"
    private const val STATE = "state"
    private const val NEXT_MS = "next_ms"
    private const val CHANNEL = "vasdk_reminders"
    const val ACTION_DUE = "com.vasdk.android.TRIGGER_DUE"

    /** The state kept when the engine last ran ("" for none), to give to the engine as it starts. */
    fun savedState(context: Context): String =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getString(STATE, "") ?: ""

    /** The next due time kept with it, in epoch ms; -1 for none. */
    fun savedNextMs(context: Context): Long =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getLong(NEXT_MS, -1)

    /** Keeps the engine's trigger state and sets the alarm for the next due time (none: cancelled). */
    fun keep(context: Context, state: String, nextMs: Long) {
        if (state.isEmpty()) return   // no agent, or not checked yet: what's kept stays
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putString(STATE, state).putLong(NEXT_MS, nextMs).apply()
        val alarms = context.getSystemService(AlarmManager::class.java) ?: return
        val due = PendingIntent.getBroadcast(context, 0, Intent(context, AlarmReceiver::class.java).setAction(ACTION_DUE),
                                             PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        if (nextMs < 0) {
            alarms.cancel(due)
            return
        }
        // Exact when the user allowed it (Settings > Alarms & reminders), else within Doze's window.
        if (alarms.canScheduleExactAlarms()) alarms.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, nextMs, due)
        else alarms.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, nextMs, due)
        Log.i(TAG, "next schedule trigger at $nextMs")
    }

    /** A notification another app posted, as the agent's `notification` app event. */
    fun onNotification(context: Context, notification: StatusBarNotification) {
        if (notification.packageName == context.packageName || notification.isOngoing) return
        val extras = notification.notification.extras
        val payload = JSONObject()
            .put("package", notification.packageName)
            .put("title", extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: "")
            .put("text", extras.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: "")
            .put("category", notification.notification.category ?: "")
        val engine = VasdkEngine.getInstance(context)
        Log.i(TAG, "notification from ${notification.packageName}: " +
                   if (engine.isStarted()) "the agent's notification event" else "the engine isn't running")
        engine.fireTrigger("notification", payload.toString())
    }

    /** The alarm: the engine says the reminder when it's running; else the user is asked to open the
     *  app. After a reboot (alarms don't survive one), the kept alarm is set again. */
    class AlarmReceiver : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == Intent.ACTION_BOOT_COMPLETED) {
                keep(context, savedState(context), savedNextMs(context))
                return
            }
            if (intent.action != ACTION_DUE) return
            val engine = VasdkEngine.getInstance(context)
            if (engine.isStarted()) {
                Log.i(TAG, "a schedule trigger is due: the running engine says it")
                return
            }
            Log.i(TAG, "a schedule trigger is due while the engine is stopped: notifying")
            val notifications = context.getSystemService(NotificationManager::class.java) ?: return
            notifications.createNotificationChannel(
                NotificationChannel(CHANNEL, "Reminders", NotificationManager.IMPORTANCE_HIGH))
            val open = PendingIntent.getActivity(context, 0,
                Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE)
            notifications.notify(CHANNEL.hashCode(), Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_popup_reminder)
                .setContentTitle("Your assistant has a reminder")
                .setContentText("Tap to hear it.")
                .setContentIntent(open)
                .setAutoCancel(true)
                .build())
        }
    }
}
