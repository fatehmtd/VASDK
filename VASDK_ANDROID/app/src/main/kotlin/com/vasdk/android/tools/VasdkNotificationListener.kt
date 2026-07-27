package com.vasdk.android.tools

import android.content.ComponentName
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.service.notification.NotificationListenerService

class VasdkNotificationListener : NotificationListenerService() {

    companion object {
        // Non-null only while the system has the listener connected (i.e. notification access granted).
        @Volatile var instance: VasdkNotificationListener? = null
    }

    override fun onListenerConnected() {
        instance = this
    }

    override fun onListenerDisconnected() {
        instance = null
    }

    fun getActiveSessions(): List<MediaController> {
        val msm = getSystemService(MEDIA_SESSION_SERVICE) as? MediaSessionManager ?: return emptyList()
        return try {
            msm.getActiveSessions(ComponentName(this, VasdkNotificationListener::class.java))
        } catch (e: Exception) {
            emptyList()
        }
    }
}
