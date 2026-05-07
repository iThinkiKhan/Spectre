package com.companion_app.spectre

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.BitmapFactory
import android.graphics.Color
import android.location.LocationManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.content.ContextCompat
import com.companion_app.R

class SpectreBleForegroundService : Service() {
  companion object {
    private const val LOG_TAG = "SpectreBleService"
    private const val CHANNEL_ID = "spectre_field_mode_v2"
    private const val NOTIFICATION_ID = 8403
    private const val ACTION_START_FIELD_MODE =
      "com.companion_app.spectre.action.START_FIELD_MODE"
    private const val ACTION_STOP_FIELD_MODE =
      "com.companion_app.spectre.action.STOP_FIELD_MODE"
    private const val EXTRA_USE_DEVICE_LOCATION = "useDeviceLocation"

    @Volatile
    private var stopRequestHandler: (() -> Unit)? = null

    fun start(context: Context, useDeviceLocation: Boolean) {
      val intent = Intent(context, SpectreBleForegroundService::class.java).apply {
        action = ACTION_START_FIELD_MODE
        putExtra(EXTRA_USE_DEVICE_LOCATION, useDeviceLocation)
      }
      ContextCompat.startForegroundService(context, intent)
    }

    fun stop(context: Context) {
      context.stopService(Intent(context, SpectreBleForegroundService::class.java))
    }

    fun registerStopRequestHandler(handler: (() -> Unit)?) {
      stopRequestHandler = handler
    }
  }

  override fun onCreate() {
    super.onCreate()
    createNotificationChannel()
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP_FIELD_MODE) {
      Log.i(LOG_TAG, "foreground notification stop requested")
      stopRequestHandler?.invoke()
      stopForegroundCompat()
      stopSelf()
      return START_NOT_STICKY
    }

    val useDeviceLocation =
      intent?.getBooleanExtra(EXTRA_USE_DEVICE_LOCATION, false) == true
    if (!hasForegroundPrerequisites(useDeviceLocation)) {
      stopSelf()
      return START_NOT_STICKY
    }

    val notification = buildNotification(useDeviceLocation)
    try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        startForeground(
          NOTIFICATION_ID,
          notification,
          foregroundServiceTypes(useDeviceLocation),
        )
      } else {
        startForeground(NOTIFICATION_ID, notification)
      }
    } catch (error: SecurityException) {
      Log.e(LOG_TAG, "foreground service permission check failed: ${error.message}", error)
      stopSelf()
      return START_NOT_STICKY
    }

    Log.i(
      LOG_TAG,
      "foreground BLE service active type=${if (useDeviceLocation) "connectedDevice|location" else "connectedDevice"}",
    )
    return START_NOT_STICKY
  }

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onTaskRemoved(rootIntent: Intent?) {
    Log.i(LOG_TAG, "task removed; keeping BLE foreground service active")
    super.onTaskRemoved(rootIntent)
  }

  override fun onDestroy() {
    Log.i(LOG_TAG, "foreground BLE service destroyed")
    super.onDestroy()
  }

  private fun foregroundServiceTypes(useDeviceLocation: Boolean): Int {
    var types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
    if (useDeviceLocation) {
      types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
    }
    return types
  }

  private fun hasForegroundPrerequisites(useDeviceLocation: Boolean): Boolean {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      if (!hasPermission(Manifest.permission.BLUETOOTH_CONNECT) ||
        !hasPermission(Manifest.permission.BLUETOOTH_ADVERTISE)
      ) {
        Log.e(LOG_TAG, "missing Bluetooth permissions for connectedDevice foreground service")
        return false
      }
    }

    if (useDeviceLocation) {
      if (!hasPermission(Manifest.permission.ACCESS_FINE_LOCATION) &&
        !hasPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
      ) {
        Log.e(LOG_TAG, "missing location permission for location foreground service")
        return false
      }
      if (!locationServicesEnabled()) {
        Log.e(LOG_TAG, "location services disabled for location foreground service")
        return false
      }
    }

    return true
  }

  private fun hasPermission(permission: String): Boolean =
    ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

  private fun locationServicesEnabled(): Boolean {
    val locationManager = getSystemService(LocationManager::class.java)
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      locationManager.isLocationEnabled
    } else {
      @Suppress("DEPRECATION")
      locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
        locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
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

  private fun createNotificationChannel() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
      return
    }

    val manager = getSystemService(NotificationManager::class.java)
    val channel = NotificationChannel(
      CHANNEL_ID,
      "Spectre Field Mode",
      NotificationManager.IMPORTANCE_LOW,
    ).apply {
      description = "Keeps the Spectre BLE companion link and phone GPS available"
      setShowBadge(false)
    }
    manager.createNotificationChannel(channel)
  }

  private fun buildNotification(useDeviceLocation: Boolean): Notification {
    val launchIntent = packageManager.getLaunchIntentForPackage(packageName)
    val largeIcon = BitmapFactory.decodeResource(resources, R.mipmap.ic_launcher)
    val pendingFlags = PendingIntent.FLAG_UPDATE_CURRENT or
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) PendingIntent.FLAG_IMMUTABLE else 0
    val contentIntent = if (launchIntent != null) {
      PendingIntent.getActivity(this, 0, launchIntent, pendingFlags)
    } else {
      null
    }
    val stopIntent = Intent(this, SpectreBleForegroundService::class.java).apply {
      action = ACTION_STOP_FIELD_MODE
    }
    val stopPendingIntent = PendingIntent.getService(
      this,
      1,
      stopIntent,
      pendingFlags,
    )

    val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      Notification.Builder(this, CHANNEL_ID)
    } else {
      @Suppress("DEPRECATION")
      Notification.Builder(this)
    }

    return builder
      .setSmallIcon(R.drawable.ic_stat_spectre)
      .setContentTitle("Spectre Field Mode")
      .setContentText(
        if (useDeviceLocation) {
          "SpectrePhone advertising and GPS are active"
        } else {
          "SpectrePhone advertising is active"
        },
      )
      .setOngoing(true)
      .setShowWhen(false)
      .setCategory(Notification.CATEGORY_SERVICE)
      .setLargeIcon(largeIcon)
      .setColor(Color.rgb(252, 231, 0))
      .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", stopPendingIntent)
      .apply {
        if (contentIntent != null) {
          setContentIntent(contentIntent)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
          setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        }
      }
      .build()
  }
}
