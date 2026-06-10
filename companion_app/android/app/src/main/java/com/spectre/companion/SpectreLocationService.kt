package com.spectre.companion

import android.Manifest
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt
import org.json.JSONArray
import org.json.JSONObject

/**
 * Field Mode GPS recorder.
 *
 * Runs only while the companion peripheral is active and phone GPS mode is
 * selected.  It writes fixes into the same @spectre/location-history-v2/day/<id>
 * SharedPreferences keys that the JS LocationHistoryStore reads on app start.
 */
class SpectreLocationService : Service(), LocationListener {

  private val handler = Handler(Looper.getMainLooper())
  private val prefs: SharedPreferences by lazy {
    applicationContext.getSharedPreferences("spectre_companion_store", Context.MODE_PRIVATE)
  }
  private val locationManager: LocationManager by lazy {
    applicationContext.getSystemService(Context.LOCATION_SERVICE) as LocationManager
  }

  // In-memory mirror of the on-disk day buckets, keyed by bucket id.  Loaded
  // lazily on first sample so we don't blow away whatever the JS already wrote.
  private val buckets = sortedMapOf<Long, MutableList<JSONObject>>()
  private var bucketsLoaded = false
  private val dirtyBuckets = mutableSetOf<Long>()
  private var persistScheduled = false
  private var lastAcceptedTimestamp: Long = 0

  override fun onCreate() {
    super.onCreate()
    SpectreFieldService.ensureNotificationChannel(this)
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP) {
      active = false
      stopForegroundCompat()
      stopSelf()
      return START_NOT_STICKY
    }

    startForegroundCompat(SpectreFieldService.buildNotification(this, true))
    active = true
    if (!hasLocationPermission()) {
      Log.w(LOG_TAG, "event=start_blocked reason=missing_location_permission")
      // We still keep the service alive so a later grant-then-restart works,
      // but without permission we can't register listeners.
      return START_STICKY
    }

    registerListeners()
    return START_STICKY
  }

  override fun onDestroy() {
    active = false
    runCatching { locationManager.removeUpdates(this) }
    flushNow()
    super.onDestroy()
  }

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onLocationChanged(location: Location) {
    if (!bucketsLoaded) {
      loadBucketsFromDisk()
      bucketsLoaded = true
    }
    val timestamp = if (location.time > 0) location.time else System.currentTimeMillis()
    // Throttle to MIN_INTERVAL_MS even if multiple providers fire — keeps the
    // backlog from filling with near-duplicates.
    if (timestamp - lastAcceptedTimestamp < MIN_INTERVAL_MS - INTERVAL_SLACK_MS) {
      return
    }

    val accuracy = if (location.hasAccuracy()) location.accuracy.toDouble() else 0.0
    if (accuracy > 0 && accuracy > MAX_ACCURACY_M) {
      // Reject obviously bad fixes (e.g., 5km network-provider guesses).
      return
    }

    val fix = JSONObject().apply {
      put("lat", location.latitude)
      put("lon", location.longitude)
      put("alt", if (location.hasAltitude()) location.altitude else 0.0)
      put("accuracy", max(0.0, accuracy))
      put("timestamp", timestamp)
      put("source", "device")
      put("provider", location.provider ?: "fused")
    }

    if (!appendFix(fix)) {
      return
    }
    lastAcceptedTimestamp = timestamp
    schedulePersist()
    SpectreLocationRecorderModule.dispatchFix(fix)
  }

  override fun onProviderEnabled(provider: String) = Unit
  override fun onProviderDisabled(provider: String) = Unit

  @Suppress("DEPRECATION")
  override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit

  private fun hasLocationPermission(): Boolean {
    val fine = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
    val coarse = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION)
    return fine == PackageManager.PERMISSION_GRANTED ||
        coarse == PackageManager.PERMISSION_GRANTED
  }

  @SuppressWarnings("MissingPermission")
  private fun registerListeners() {
    val providers = listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)
    for (provider in providers) {
      try {
        if (!locationManager.isProviderEnabled(provider)) {
          continue
        }
        @Suppress("MissingPermission")
        locationManager.requestLocationUpdates(
            provider,
            MIN_INTERVAL_MS,
            MIN_DISTANCE_M,
            this,
            Looper.getMainLooper(),
        )
        Log.i(LOG_TAG, "event=listener_registered provider=$provider intervalMs=$MIN_INTERVAL_MS")
      } catch (error: SecurityException) {
        Log.w(LOG_TAG, "event=listener_register_failed provider=$provider reason=security")
      } catch (error: IllegalArgumentException) {
        Log.w(LOG_TAG, "event=listener_register_failed provider=$provider reason=${error.message}")
      }
    }

    // Seed an immediate last-known-fix so the backlog gets a marker even before
    // the first provider tick lands.
    seedFromLastKnown()
  }

  private fun seedFromLastKnown() {
    val providers = listOf(
        LocationManager.GPS_PROVIDER,
        LocationManager.NETWORK_PROVIDER,
        LocationManager.PASSIVE_PROVIDER,
    )
    for (provider in providers) {
      try {
        if (!locationManager.isProviderEnabled(provider)) continue
        @Suppress("MissingPermission")
        val location = locationManager.getLastKnownLocation(provider) ?: continue
        onLocationChanged(location)
        return
      } catch (_: SecurityException) {
        continue
      }
    }
  }

  private fun appendFix(fix: JSONObject): Boolean {
    val timestamp = fix.optLong("timestamp", 0L)
    if (timestamp <= 0) return false
    val bucketId = timestamp / DAY_MS
    val bucket = buckets.getOrPut(bucketId) { mutableListOf() }

    // Coalesce against the most recent same-source marker — mirrors
    // rememberLocationSample in LocationHistoryStore.ts so we don't grow the
    // backlog while stationary.
    val previousMarker = findPreviousMarker(bucketId, "device")
    if (previousMarker != null && withinAccuracyRadius(previousMarker, fix)) {
      val prevAccuracy = previousMarker.optDouble("accuracy", 0.0)
      val newAccuracy = fix.optDouble("accuracy", 0.0)
      if (newAccuracy > 0 && (prevAccuracy == 0.0 || newAccuracy < prevAccuracy)) {
        previousMarker.put("lat", fix.optDouble("lat"))
        previousMarker.put("lon", fix.optDouble("lon"))
        previousMarker.put("alt", fix.optDouble("alt"))
        previousMarker.put("accuracy", newAccuracy)
        previousMarker.put("provider", fix.opt("provider"))
        dirtyBuckets.add(previousMarker.optLong("timestamp") / DAY_MS)
      }
      return false
    }

    bucket.add(fix)
    dirtyBuckets.add(bucketId)
    enforceCaps()
    return true
  }

  private fun findPreviousMarker(currentBucketId: Long, source: String): JSONObject? {
    val ids = buckets.keys.filter { it <= currentBucketId }.sortedDescending()
    for (id in ids) {
      val list = buckets[id] ?: continue
      for (i in list.indices.reversed()) {
        if (list[i].optString("source") == source) {
          return list[i]
        }
      }
    }
    return null
  }

  private fun enforceCaps() {
    val cutoff = System.currentTimeMillis() - MAX_AGE_MS
    val cutoffBucket = cutoff / DAY_MS
    val expired = buckets.keys.filter { it < cutoffBucket - 1 }.toList()
    for (id in expired) {
      buckets.remove(id)
      dirtyBuckets.add(id) // marks for delete on flush
    }

    var total = 0
    for (list in buckets.values) total += list.size
    if (total <= MAX_SAMPLES) return

    val excess = total - MAX_SAMPLES
    var remaining = excess
    val sortedIds = buckets.keys.sorted()
    for (id in sortedIds) {
      if (remaining <= 0) break
      val list = buckets[id] ?: continue
      val drop = minOf(remaining, list.size)
      if (drop == list.size) {
        buckets.remove(id)
      } else {
        for (i in 0 until drop) list.removeAt(0)
      }
      dirtyBuckets.add(id)
      remaining -= drop
    }
  }

  private fun loadBucketsFromDisk() {
    try {
      val indexRaw = prefs.getString(INDEX_KEY, null) ?: return
      val indexArray = JSONArray(indexRaw)
      for (i in 0 until indexArray.length()) {
        val id = indexArray.optLong(i, Long.MIN_VALUE)
        if (id == Long.MIN_VALUE) continue
        val bucketRaw = prefs.getString(bucketKey(id), null) ?: continue
        val bucketArray = JSONArray(bucketRaw)
        val list = mutableListOf<JSONObject>()
        for (j in 0 until bucketArray.length()) {
          val fix = bucketArray.optJSONObject(j) ?: continue
          list.add(fix)
        }
        if (list.isNotEmpty()) {
          buckets[id] = list
        }
      }
      Log.i(LOG_TAG, "event=buckets_loaded buckets=${buckets.size}")
    } catch (error: Exception) {
      Log.w(LOG_TAG, "event=buckets_load_failed reason=${error.message}")
    }
  }

  private fun schedulePersist() {
    if (persistScheduled) return
    persistScheduled = true
    handler.postDelayed({
      persistScheduled = false
      flushNow()
    }, PERSIST_DEBOUNCE_MS)
  }

  private fun flushNow() {
    if (dirtyBuckets.isEmpty()) return
    val editor = prefs.edit()

    val priorIndex = mutableSetOf<Long>()
    try {
      val indexRaw = prefs.getString(INDEX_KEY, null)
      if (indexRaw != null) {
        val arr = JSONArray(indexRaw)
        for (i in 0 until arr.length()) {
          val v = arr.optLong(i, Long.MIN_VALUE)
          if (v != Long.MIN_VALUE) priorIndex.add(v)
        }
      }
    } catch (_: Exception) {
      // Treat as empty.
    }

    for (id in dirtyBuckets.toList()) {
      val list = buckets[id]
      if (list == null || list.isEmpty()) {
        editor.remove(bucketKey(id))
      } else {
        val arr = JSONArray()
        for (fix in list) arr.put(fix)
        editor.putString(bucketKey(id), arr.toString())
      }
    }
    dirtyBuckets.clear()

    val sortedLiveIds = buckets.keys.sorted()
    val indexChanged =
        priorIndex.size != sortedLiveIds.size ||
            sortedLiveIds.any { !priorIndex.contains(it) } ||
            priorIndex.any { !sortedLiveIds.contains(it) }
    if (indexChanged) {
      val arr = JSONArray()
      for (id in sortedLiveIds) arr.put(id)
      editor.putString(INDEX_KEY, arr.toString())
    }
    editor.apply()
    Log.i(LOG_TAG, "event=flush buckets=${buckets.size}")
  }

  private fun bucketKey(id: Long): String = "$BUCKET_KEY_PREFIX$id"

  private fun withinAccuracyRadius(a: JSONObject, b: JSONObject): Boolean {
    val accA = a.optDouble("accuracy", 0.0)
    val accB = b.optDouble("accuracy", 0.0)
    val threshold = max(max(accA, accB), 0.0)
    return distanceMeters(a, b) <= threshold
  }

  private fun distanceMeters(a: JSONObject, b: JSONObject): Double {
    val radiusM = 6_371_000.0
    val latA = a.optDouble("lat", 0.0)
    val lonA = a.optDouble("lon", 0.0)
    val latB = b.optDouble("lat", 0.0)
    val lonB = b.optDouble("lon", 0.0)
    val lat1 = Math.toRadians(latA)
    val lat2 = Math.toRadians(latB)
    val deltaLat = Math.toRadians(latB - latA)
    val deltaLon = Math.toRadians(lonB - lonA)
    val sinLat = sin(deltaLat / 2)
    val sinLon = sin(deltaLon / 2)
    val h = sinLat * sinLat + cos(lat1) * cos(lat2) * sinLon * sinLon
    return 2 * radiusM * atan2(sqrt(h), sqrt(1 - h))
  }

  private fun stopForegroundCompat() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
      stopForeground(STOP_FOREGROUND_DETACH)
    } else {
      @Suppress("DEPRECATION")
      stopForeground(false)
    }
  }

  private fun startForegroundCompat(notification: android.app.Notification) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      startForeground(
          SpectreFieldService.NOTIFICATION_ID,
          notification,
          ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
      )
    } else {
      startForeground(SpectreFieldService.NOTIFICATION_ID, notification)
    }
  }

  companion object {
    const val ACTION_STOP = "com.spectre.companion.action.STOP_LOCATION_RECORDER"
    private const val LOG_TAG = "SpectreLocationSvc"
    @Volatile private var active = false

    // Mirrors LOCATION_HISTORY_* constants in LocationHistoryStore.ts.
    private const val DAY_MS = 24L * 60 * 60 * 1000
    private const val MAX_AGE_MS = 30L * DAY_MS
    private const val MAX_SAMPLES = 200_000
    private const val MIN_INTERVAL_MS = 10_000L
    private const val INTERVAL_SLACK_MS = 500L
    private const val MIN_DISTANCE_M = 0f
    private const val PERSIST_DEBOUNCE_MS = 30_000L
    private const val MAX_ACCURACY_M = 5_000.0

    private const val INDEX_KEY = "@spectre/location-history-v2/index"
    private const val BUCKET_KEY_PREFIX = "@spectre/location-history-v2/day/"

    fun start(context: Context) {
      val intent = Intent(context, SpectreLocationService::class.java)
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
        context.startForegroundService(intent)
      } else {
        context.startService(intent)
      }
    }

    fun stop(context: Context) {
      if (!active) {
        return
      }
      val intent = Intent(context, SpectreLocationService::class.java).apply {
        action = ACTION_STOP
      }
      runCatching {
        context.startService(intent)
      }.onFailure {
        context.stopService(Intent(context, SpectreLocationService::class.java))
      }
    }
  }
}
