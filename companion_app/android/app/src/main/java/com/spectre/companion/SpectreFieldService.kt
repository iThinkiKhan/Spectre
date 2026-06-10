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

class SpectreFieldService : Service() {
  override fun onCreate() {
    super.onCreate()
    ensureNotificationChannel(this)
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP_FIELD_MODE) {
      SpectrePeripheralModule.stopFromNotification()
      stopForegroundCompat()
      stopSelf()
      return START_NOT_STICKY
    }

    // The foreground service alone keeps this process alive; Android wakes
    // the CPU for incoming GATT events.  No PARTIAL_WAKE_LOCK — that just
    // pins the CPU at 100% duty for the entire Field Mode session.
    startForeground(NOTIFICATION_ID, buildNotification(this))
    SpectrePeripheralModule.kickFromFieldService()
    return START_STICKY
  }

  override fun onBind(intent: Intent?): IBinder? = null

  private fun stopForegroundCompat() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
      stopForeground(STOP_FOREGROUND_REMOVE)
    } else {
      @Suppress("DEPRECATION")
      stopForeground(true)
    }
  }

  companion object {
    const val CHANNEL_ID = "spectre_field_mode"
    const val ACTION_STOP_FIELD_MODE = "com.spectre.companion.action.STOP_FIELD_MODE"
    const val NOTIFICATION_ID = 4201

    fun ensureNotificationChannel(context: Context) {
      if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
        return
      }

      val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
      val channel =
          NotificationChannel(
              CHANNEL_ID,
              context.getString(R.string.field_service_channel),
              NotificationManager.IMPORTANCE_LOW,
          ).apply {
            description = context.getString(R.string.field_service_channel_description)
            setShowBadge(false)
          }
      manager.createNotificationChannel(channel)
    }

    fun buildNotification(context: Context): Notification {
      val launchIntent =
          context.packageManager.getLaunchIntentForPackage(context.packageName)?.apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
          }
      val pendingIntent =
          PendingIntent.getActivity(
              context,
              0,
              launchIntent,
              PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
          )

      val builder =
          if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(context, CHANNEL_ID)
          } else {
            @Suppress("DEPRECATION")
            Notification.Builder(context)
          }

      return builder
          .setSmallIcon(R.drawable.ic_stat_spectre)
          .setContentTitle(context.getString(R.string.field_service_title))
          .setContentText(context.getString(R.string.field_service_text))
          .setOngoing(true)
          .setShowWhen(false)
          .setContentIntent(pendingIntent)
          .addAction(stopNotificationAction(context))
          .build()
    }

    private fun stopNotificationAction(context: Context): Notification.Action =
        Notification.Action.Builder(
            Icon.createWithResource(context, R.drawable.ic_stat_spectre_stop),
            context.getString(R.string.field_service_stop_action),
            stopPendingIntent(context),
        ).build()

    private fun stopPendingIntent(context: Context): PendingIntent =
        PendingIntent.getService(
            context,
            1,
            Intent(context, SpectreFieldService::class.java).apply {
              action = ACTION_STOP_FIELD_MODE
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
  }
}
