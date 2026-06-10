package com.spectre.companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper

class SpectreFieldService : Service() {
  private val handler = Handler(Looper.getMainLooper())
  private var gpsLogging = false
  private val serviceKick =
      object : Runnable {
        override fun run() {
          SpectrePeripheralModule.kickFromFieldService()
          handler.postDelayed(this, SERVICE_KICK_INTERVAL_MS)
        }
      }

  override fun onCreate() {
    super.onCreate()
    ensureNotificationChannel(this)
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP_FIELD_MODE) {
      SpectrePeripheralModule.stopFromNotification()
      handler.removeCallbacks(serviceKick)
      stopForegroundCompat()
      stopSelf()
      return START_NOT_STICKY
    }

    gpsLogging = intent?.getBooleanExtra(EXTRA_GPS_LOGGING, gpsLogging) ?: gpsLogging
    // The foreground service alone keeps this process alive; Android wakes
    // the CPU for incoming GATT events.  No PARTIAL_WAKE_LOCK — that just
    // pins the CPU at 100% duty for the entire Field Mode session.
    startForegroundCompat(buildNotification(this, gpsLogging), gpsLogging)
    SpectrePeripheralModule.kickFromFieldService()
    handler.removeCallbacks(serviceKick)
    handler.postDelayed(serviceKick, SERVICE_KICK_INTERVAL_MS)
    return START_STICKY
  }

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onDestroy() {
    handler.removeCallbacks(serviceKick)
    super.onDestroy()
  }

  private fun startForegroundCompat(notification: Notification, includeLocation: Boolean) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      var serviceType = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
      if (includeLocation) {
        serviceType = serviceType or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
      }
      startForeground(NOTIFICATION_ID, notification, serviceType)
    } else {
      startForeground(NOTIFICATION_ID, notification)
    }
  }

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
    const val EXTRA_GPS_LOGGING = "com.spectre.companion.extra.GPS_LOGGING"
    const val NOTIFICATION_ID = 4201
    private const val SERVICE_KICK_INTERVAL_MS = 5_000L

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

    fun buildNotification(context: Context, gpsLogging: Boolean = true): Notification {
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
      val contentText =
          context.getString(
              if (gpsLogging) {
                R.string.field_service_text_ble_gps
              } else {
                R.string.field_service_text_ble_only
              }
          )

      return builder
          .setSmallIcon(R.drawable.ic_stat_spectre)
          .setContentTitle(context.getString(R.string.field_service_title))
          .setContentText(contentText)
          .setStyle(Notification.BigTextStyle().bigText(contentText))
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
