package com.vasdk.android.services

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.vasdk.android.VasdkEngine

class ModelDownloadService : Service() {

    private val engine by lazy { VasdkEngine.getInstance(applicationContext) }
    private var isDownloading = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (engine.isReady()) {
            broadcast(ACTION_MODELS_READY, true)
            stopSelf()
            return START_NOT_STICKY
        }

        if (isDownloading) return START_NOT_STICKY
        isDownloading = true

        startForeground(
            NOTIFICATION_ID,
            buildNotification(0f, 0f, "Preparing models..."),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
        )

        engine.downloadAndInit(
            onProgress = { fileProgress, overallProgress, fileName ->
                NotificationManagerCompat.from(this)
                    .notify(NOTIFICATION_ID, buildNotification(fileProgress, overallProgress, fileName))
                sendBroadcast(
                    Intent(ACTION_DOWNLOAD_PROGRESS)
                        .setPackage(packageName)
                        .putExtra(EXTRA_FILE_PROGRESS, fileProgress)
                        .putExtra(EXTRA_OVERALL_PROGRESS, overallProgress)
                        .putExtra(EXTRA_FILE_NAME, fileName)
                )
            }
        ) { success ->
            isDownloading = false
            broadcast(ACTION_MODELS_READY, success)
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }

        return START_NOT_STICKY
    }

    private fun broadcast(action: String, success: Boolean) {
        sendBroadcast(
            Intent(action)
                .setPackage(packageName)
                .putExtra(EXTRA_SUCCESS, success)
        )
    }

    private fun buildNotification(fileProgress: Float, overallProgress: Float, fileName: String): Notification {
        val filePct = (fileProgress * 100f).toInt().coerceIn(0, 100)
        val overallPct = (overallProgress * 100f).toInt().coerceIn(0, 100)
        val text = if (overallPct < 100) "Downloading $fileName ($filePct%) - overall $overallPct%" else "Initializing models..."
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("VASDK")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setProgress(100, overallPct, overallPct == 0)
            .setOngoing(true)
            .setSilent(true)
            .build()
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Model Downloads",
            NotificationManager.IMPORTANCE_LOW
        ).apply { setShowBadge(false) }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    companion object {
        const val ACTION_MODELS_READY = "com.vasdk.android.ACTION_MODELS_READY"
        const val ACTION_DOWNLOAD_PROGRESS = "com.vasdk.android.ACTION_DOWNLOAD_PROGRESS"
        const val EXTRA_SUCCESS = "success"
        const val EXTRA_FILE_PROGRESS = "file_progress"
        const val EXTRA_OVERALL_PROGRESS = "overall_progress"
        const val EXTRA_FILE_NAME = "file_name"
        private const val CHANNEL_ID = "vasdk_download"
        private const val NOTIFICATION_ID = 1001
    }
}
