package com.vasdk.android.tools

import android.content.Context
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.PlaybackState
import android.os.Build
import android.provider.MediaStore
import android.view.KeyEvent
import org.json.JSONArray
import org.json.JSONObject

object MediaTools {
    val toolNames = setOf(
        "get_volume",
        "set_volume",
        "set_flashlight",
        "media_play",
        "media_pause",
        "media_play_pause",
        "media_next_track",
        "media_prev_track",
        "get_now_playing",
        "get_all_volumes",
        "set_volume_stream"
    )

    fun owns(toolName: String) = toolName in toolNames
    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "get_volume" -> getVolume(context)
        "set_volume" -> setVolume(context, argsJson)
        "set_flashlight" -> setFlashlight(context, argsJson)
        "media_play" -> dispatchMediaKey(context, KeyEvent.KEYCODE_MEDIA_PLAY, "play")
        "media_pause" -> dispatchMediaKey(context, KeyEvent.KEYCODE_MEDIA_PAUSE, "pause")
        "media_play_pause" -> dispatchMediaKey(
            context,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            "play_pause"
        )
        "media_next_track" -> dispatchMediaKey(context, KeyEvent.KEYCODE_MEDIA_NEXT, "next_track")
        "media_prev_track" -> dispatchMediaKey(
            context,
            KeyEvent.KEYCODE_MEDIA_PREVIOUS,
            "prev_track"
        )
        "get_now_playing" -> getNowPlaying(context)
        "get_all_volumes" -> getAllVolumes(context)
        "set_volume_stream" -> setVolumeStream(context, argsJson)
        else -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    //  Volume
    private fun getVolume(context: Context): String {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        return JSONObject().apply {
            put("current", am.getStreamVolume(AudioManager.STREAM_MUSIC))
            put("max", am.getStreamMaxVolume(AudioManager.STREAM_MUSIC))
        }.toString()
    }

    private fun setVolume(context: Context, argsJson: String): String {
        val args = JSONObject(argsJson)
        val level = args.getInt("level")
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
        val clamped = level.coerceIn(0, max)
        am.setStreamVolume(AudioManager.STREAM_MUSIC, clamped, 0)
        return JSONObject().apply {
            put("set_to", clamped)
            put("max", max)
        }.toString()
    }

    //  On/off argument parsing

    // Accepts a real boolean as well as the string/number spellings local models
    // tend to produce ("on", "true", 1, ...). Returns null when missing or unrecognized.
    private fun parseOnOff(args: JSONObject, key: String): Boolean? = when (val value = args.opt(key)) {
        is Boolean -> value
        is Number -> value.toInt() != 0
        is String -> when (value.trim().lowercase()) {
            "on", "true", "yes", "1" -> true
            "off", "false", "no", "0" -> false
            else -> null
        }
        else -> null
    }

    //  Flashlight
    private fun flashCameraId(cm: CameraManager): String? =
        cm.cameraIdList.firstOrNull { id ->
            cm.getCameraCharacteristics(id).get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
        }

    private fun setFlashlight(context: Context, argsJson: String): String {
        val on = parseOnOff(JSONObject(argsJson), "on")
            ?: return JSONObject().put("error", "Missing or invalid 'on' argument, expected true or false").toString()
        val cm = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        return try {
            val cameraId = flashCameraId(cm)
                ?: return JSONObject().put("error", "No camera with flash found").toString()
            cm.setTorchMode(cameraId, on)
            JSONObject().put("flashlight", if (on) "on" else "off").toString()
        } catch (e: CameraAccessException) {
            JSONObject().put("error", "Camera error: ${e.message}").toString()
        }
    }

    //  Media playback control 

    private fun dispatchMediaKey(context: Context, keyCode: Int, action: String): String {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keyCode))
        am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keyCode))
        return JSONObject().apply {
            put("dispatched", true)
            put("action", action)
        }.toString()
    }

    //  Now playing 

    private fun getNowPlaying(context: Context): String {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

        // VasdkNotificationListener.instance is non-null only when the system has
        // connected the listener, which requires notification access to be granted.
        val listener = VasdkNotificationListener.instance
        if (listener == null) {
            return JSONObject().apply {
                put("music_active", am.isMusicActive)
                put(
                    "error",
                    "Notification access not granted. Enable VASDK in Settings → Apps → Special app access → Notification access, then restart the app."
                )
            }.toString()
        }

        val sessions = listener.getActiveSessions()
        if (sessions.isEmpty()) {
            return JSONObject().apply {
                put("music_active", am.isMusicActive)
                put("sessions_found", 0)
            }.toString()
        }

        val ctrl = sessions[0]
        val meta = ctrl.metadata
        val pb = ctrl.playbackState
        return JSONObject().apply {
            put("music_active", true)
            put("title", meta?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: "")
            put("artist", meta?.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: "")
            put("album", meta?.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: "")
            put("duration_ms", meta?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L)
            put("position_ms", pb?.position ?: 0L)
            put(
                "state", when (pb?.state) {
                    PlaybackState.STATE_PLAYING -> "playing"
                    PlaybackState.STATE_PAUSED -> "paused"
                    PlaybackState.STATE_STOPPED -> "stopped"
                    else -> "unknown"
                }
            )
            put("app_package", ctrl.packageName)
        }.toString()
    }

    //  Multi-stream volume 

    private val streamMap = mapOf(
        "music" to AudioManager.STREAM_MUSIC,
        "ringtone" to AudioManager.STREAM_RING,
        "alarm" to AudioManager.STREAM_ALARM,
        "notification" to AudioManager.STREAM_NOTIFICATION,
        "call" to AudioManager.STREAM_VOICE_CALL
    )

    private fun getAllVolumes(context: Context): String {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        return JSONObject().apply {
            for ((name, stream) in streamMap) {
                put(name, JSONObject().apply {
                    put("current", am.getStreamVolume(stream))
                    put("max", am.getStreamMaxVolume(stream))
                })
            }
        }.toString()
    }

    private fun setVolumeStream(context: Context, argsJson: String): String {
        val args = JSONObject(argsJson)
        val streamName = args.getString("stream")
        val level = args.getInt("level")
        val stream = streamMap[streamName]
            ?: return JSONObject().put(
                "error",
                "Unknown stream: $streamName. Use: music/ringtone/alarm/notification/call"
            ).toString()
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val max = am.getStreamMaxVolume(stream)
        val clamped = level.coerceIn(0, max)
        am.setStreamVolume(stream, clamped, 0)
        return JSONObject().apply {
            put("stream", streamName)
            put("set_to", clamped)
            put("max", max)
        }.toString()
    }
}
