package com.spectre.companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

class SpectreFieldService : Service() {
  private var wakeLock: PowerManager.WakeLock? = null

  override fun onCreate() {
    super.onCreate()
    ensureNotificationChannel()
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP_FIELD_MODE) {
      SpectrePeripheralModule.stopFromNotification()
      stopForegroundCompat()
      stopSelf()
      return START_NOT_STICKY
    }

    startForeground(NOTIFICATION_ID, buildNotification())
    acquireWakeLock()
    SpectrePeripheralModule.kickFromFieldService()
    return START_STICKY
  }

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onDestroy() {
    releaseWakeLock()
    super.onDestroy()
  }

  private fun buildNotification(): Notification {
    val launchIntent =
        packageManager.getLaunchIntentForPackage(packageName)?.apply {
          flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
    val pendingIntent =
        PendingIntent.getActivity(
            this,
            0,
            launchIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

    val builder =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
          Notification.Builder(this, CHANNEL_ID)
        } else {
          @Suppress("DEPRECATION")
          Notification.Builder(this)
        }

    return builder
        .setSmallIcon(R.drawable.ic_stat_spectre)
        .setContentTitle(getString(R.string.field_service_title))
        .setContentText(getString(R.string.field_service_text))
        .setOngoing(true)
        .setShowWhen(false)
        .setContentIntent(pendingIntent)
        .addAction(stopNotificationAction())
        .build()
  }

  private fun stopNotificationAction(): Notification.Action =
      Notification.Action.Builder(
          Icon.createWithResource(this, R.drawable.ic_stat_spectre_stop),
          getString(R.string.field_service_stop_action),
          stopPendingIntent(),
      ).build()

  private fun stopPendingIntent(): PendingIntent =
      PendingIntent.getService(
          this,
          1,
          Intent(this, SpectreFieldService::class.java).apply {
            action = ACTION_STOP_FIELD_MODE
          },
          PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
      )

  private fun ensureNotificationChannel() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
      return
    }

    val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    val channel =
        NotificationChannel(
            CHANNEL_ID,
            getString(R.string.field_service_channel),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
          description = getString(R.string.field_service_channel_description)
          setShowBadge(false)
        }
    manager.createNotificationChannel(channel)
  }

  private fun stopForegroundCompat() {
    releaseWakeLock()
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
      stopForeground(STOP_FOREGROUND_REMOVE)
    } else {
      @Suppress("DEPRECATION")
      stopForeground(true)
    }
  }

  private fun acquireWakeLock() {
    val existing = wakeLock
    if (existing?.isHeld == true) {
      return
    }

    val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
    wakeLock =
        powerManager
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "$packageName:SpectreFieldMode")
            .apply {
              setReferenceCounted(false)
              acquire()
            }
  }

  private fun releaseWakeLock() {
    wakeLock?.let {
      if (it.isHeld) {
        it.release()
      }
    }
    wakeLock = null
  }

  companion object {
    const val CHANNEL_ID = "spectre_field_mode"
    const val ACTION_STOP_FIELD_MODE = "com.spectre.companion.action.STOP_FIELD_MODE"
    private const val NOTIFICATION_ID = 4201
  }
}
