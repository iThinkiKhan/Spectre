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
import android.util.Log

class SpectreFieldService : Service() {
  data class FieldStatus(
      val advertising: Boolean,
      val connected: Int,
      val gpsLogging: Boolean,
  )

  private val handler = Handler(Looper.getMainLooper())
  // Single foreground-service anchor for independently toggled BLE and GPS.
  private var bleActive = false
  private var gpsActive = false
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
    when (intent?.action) {
      ACTION_STOP_FIELD_MODE -> {
        // Notification Stop tears down both subsystems.
        bleActive = false
        gpsActive = false
        SpectrePeripheralModule.stopFromNotification()
        SpectreLocationService.stop(applicationContext)
        standDown()
        return START_NOT_STICKY
      }
      ACTION_STAND_DOWN -> {
        bleActive = false
        gpsActive = false
        standDown()
        return START_NOT_STICKY
      }
      ACTION_SET_BLE ->
          bleActive = intent.getBooleanExtra(EXTRA_ACTIVE, bleActive)
      ACTION_SET_GPS ->
          gpsActive = intent.getBooleanExtra(EXTRA_ACTIVE, gpsActive)
    }

    reconcile()
    return START_NOT_STICKY
  }

  // START_NOT_STICKY: a bare restarted anchor cannot restore GATT or GPS.
  private fun reconcile() {
    if (!bleActive && !gpsActive) {
      standDown()
      return
    }

    startForegroundCompat(buildNotification(this, bleActive, gpsActive), bleActive, gpsActive)

    if (bleActive) {
      // Re-advertise heartbeat stays off in GPS-only mode.
      SpectrePeripheralModule.kickFromFieldService()
      handler.removeCallbacks(serviceKick)
      handler.postDelayed(serviceKick, SERVICE_KICK_INTERVAL_MS)
    } else {
      handler.removeCallbacks(serviceKick)
    }
  }

  private fun standDown() {
    Log.i("SpectreFieldService", "standDown begin")
    handler.removeCallbacks(serviceKick)
    // REMOVE tears down the foreground notification with the service.
    stopForegroundCompat()
    clearNotification(this)
    stopSelf()
    Log.i("SpectreFieldService", "standDown done")
  }

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onDestroy() {
    handler.removeCallbacks(serviceKick)
    stopForegroundCompat()
    super.onDestroy()
  }

  private fun startForegroundCompat(
      notification: Notification,
      includeConnectedDevice: Boolean,
      includeLocation: Boolean,
  ) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      var serviceType = 0
      if (includeConnectedDevice) {
        serviceType = serviceType or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
      }
      if (includeLocation) {
        serviceType = serviceType or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
      }
      try {
        startForeground(NOTIFICATION_ID, notification, serviceType)
      } catch (error: Exception) {
        // Keep the service alive if Android rejects a typed promotion.
        Log.w("SpectreFieldService", "typed startForeground failed (${error.javaClass.simpleName}); retrying typeless")
        startForeground(NOTIFICATION_ID, notification)
      }
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
    const val ACTION_STAND_DOWN = "com.spectre.companion.action.STAND_DOWN"
    const val ACTION_SET_BLE = "com.spectre.companion.action.SET_BLE"
    const val ACTION_SET_GPS = "com.spectre.companion.action.SET_GPS"
    const val EXTRA_ACTIVE = "com.spectre.companion.extra.ACTIVE"
    const val NOTIFICATION_ID = 4201
    // Belt-and-braces liveness poke for the peripheral module, which runs its
    // own adaptive advertising watchdog. At 5s this re-armed that watchdog
    // often enough that it could never fire on its own, and cost ~17k
    // main-thread wakeups a day for a check that is almost always a no-op.
    private const val SERVICE_KICK_INTERVAL_MS = 30_000L

    fun setBleActive(context: Context, active: Boolean) {
      updateFlag(context, ACTION_SET_BLE, active)
    }

    fun setGpsActive(context: Context, active: Boolean) {
      updateFlag(context, ACTION_SET_GPS, active)
    }

    private fun updateFlag(context: Context, action: String, active: Boolean) {
      val intent =
          Intent(context, SpectreFieldService::class.java).apply {
            this.action = action
            putExtra(EXTRA_ACTIVE, active)
          }
      runCatching {
        if (active && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
          context.startForegroundService(intent)
        } else {
          context.startService(intent)
        }
      }
    }

    fun clearNotification(context: Context) {
      val manager =
          context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
      manager.cancel(NOTIFICATION_ID)
    }

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

    fun buildNotification(
        context: Context,
        bleActive: Boolean = true,
        gpsActive: Boolean = true,
    ): Notification {
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
              when {
                bleActive && gpsActive -> R.string.field_service_text_ble_gps
                gpsActive -> R.string.field_service_text_gps_only
                else -> R.string.field_service_text_ble_only
              }
          )

      return builder
          .setSmallIcon(R.drawable.ic_stat_spectre)
          .setContentTitle(context.getString(R.string.field_service_title))
          .setContentText(contentText)
          .setStyle(Notification.BigTextStyle().bigText(contentText))
          // Let stopForeground(REMOVE) own lifecycle; OEM ongoing flags stick.
          .setShowWhen(false)
          .setContentIntent(pendingIntent)
          .addAction(stopNotificationAction(context))
          .build()
    }

    private fun statusContentText(context: Context, status: FieldStatus): String {
      val link =
          when {
            status.connected > 0 -> context.getString(R.string.field_status_linked)
            status.advertising -> context.getString(R.string.field_status_advertising)
            else -> context.getString(R.string.field_status_ble_down)
          }
      val gps =
          context.getString(
              if (status.gpsLogging) R.string.field_status_gps_on
              else R.string.field_status_gps_off
          )
      return "$link · $gps"
    }

    fun buildNotification(context: Context, status: FieldStatus): Notification {
      val contentText = statusContentText(context, status)
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
          .setContentText(contentText)
          .setStyle(Notification.BigTextStyle().bigText(contentText))
          // Let stopForeground(REMOVE) own lifecycle; OEM ongoing flags stick.
          .setShowWhen(false)
          .setContentIntent(pendingIntent)
          .addAction(stopNotificationAction(context))
          .build()
    }

    fun pushFieldNotification(context: Context, status: FieldStatus) {
      ensureNotificationChannel(context)
      val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
      manager.notify(NOTIFICATION_ID, buildNotification(context, status))
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
