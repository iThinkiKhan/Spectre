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
import com.google.android.gms.common.ConnectionResult
import com.google.android.gms.common.GoogleApiAvailability
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt
import org.json.JSONArray
import org.json.JSONObject

/** Field Mode GPS recorder writing the same buckets JS reads on startup. */
class SpectreLocationService : Service(), LocationListener {

  private val handler = Handler(Looper.getMainLooper())
  private val prefs: SharedPreferences by lazy {
    applicationContext.getSharedPreferences("spectre_companion_store", Context.MODE_PRIVATE)
  }
  private val locationManager: LocationManager by lazy {
    applicationContext.getSystemService(Context.LOCATION_SERVICE) as LocationManager
  }

  // Prefer fused; raw LocationManager is the no-Play-Services fallback.
  private var fusedClient: FusedLocationProviderClient? = null
  private var fusedCallback: LocationCallback? = null

  // Loaded lazily so we do not clobber JS-written buckets.
  private val buckets = sortedMapOf<Long, MutableList<JSONObject>>()
  private var bucketsLoaded = false
  private val dirtyBuckets = mutableSetOf<Long>()
  private var persistScheduled = false
  private var lastAcceptedTimestamp: Long = 0

  private data class BucketSummary(
      val samples: Int,
      val oldest: Long,
      val newest: Long,
  )

  override fun onCreate() {
    super.onCreate()
    SpectreFieldService.ensureNotificationChannel(this)
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP) {
      active = false
      stopSelf()
      return START_NOT_STICKY
    }

    // Plain service: SpectreFieldService owns the foreground notification.
    active = true
    if (!hasLocationPermission()) {
      Log.w(LOG_TAG, "event=start_blocked reason=missing_location_permission")
      return START_NOT_STICKY
    }

    registerListeners()
    return START_NOT_STICKY
  }

  override fun onDestroy() {
    active = false
    removeUpdates()
    flushNow()
    super.onDestroy()
  }

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onLocationChanged(location: Location) {
    ingestLocation(location)
  }

  private fun ingestLocation(location: Location) {
    if (!bucketsLoaded) {
      loadBucketsFromDisk()
      bucketsLoaded = true
    }
    val timestamp = if (location.time > 0) location.time else System.currentTimeMillis()
    // Throttle duplicate provider bursts.
    if (timestamp - lastAcceptedTimestamp < MIN_INTERVAL_MS - INTERVAL_SLACK_MS) {
      return
    }

    val accuracy = if (location.hasAccuracy()) location.accuracy.toDouble() else 0.0
    if (accuracy > 0 && accuracy > MAX_ACCURACY_M) {
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
    if (playServicesAvailable() && registerFused()) {
      seedFromLastKnown()
      return
    }
    registerGpsProvider()
    seedFromLastKnown()
  }

  private fun playServicesAvailable(): Boolean =
      runCatching {
        GoogleApiAvailability.getInstance().isGooglePlayServicesAvailable(this) ==
            ConnectionResult.SUCCESS
      }.getOrDefault(false)

  @SuppressWarnings("MissingPermission")
  private fun registerFused(): Boolean {
    return try {
      val client = LocationServices.getFusedLocationProviderClient(this)
      // Slow, batched, high-accuracy fixes save wakeups without coarsening data.
      val request =
          LocationRequest.Builder(REQUEST_PRIORITY, REQUEST_INTERVAL_MS)
              .setMinUpdateIntervalMillis(REQUEST_INTERVAL_MS)
              .setMinUpdateDistanceMeters(REQUEST_MIN_DISTANCE_M)
              .setMaxUpdateDelayMillis(REQUEST_MAX_BATCH_DELAY_MS)
              .setWaitForAccurateLocation(false)
              .build()
      val callback =
          object : LocationCallback() {
            override fun onLocationResult(result: LocationResult) {
              for (location in result.locations) {
                ingestLocation(location)
              }
            }
          }
      client.requestLocationUpdates(request, callback, Looper.getMainLooper())
      fusedClient = client
      fusedCallback = callback
      Log.i(
          LOG_TAG,
          "event=fused_registered intervalMs=$REQUEST_INTERVAL_MS minDistanceM=$REQUEST_MIN_DISTANCE_M maxBatchMs=$REQUEST_MAX_BATCH_DELAY_MS",
      )
      true
    } catch (error: SecurityException) {
      Log.w(LOG_TAG, "event=fused_register_failed reason=security")
      false
    } catch (error: Exception) {
      Log.w(LOG_TAG, "event=fused_register_failed reason=${error.message}")
      false
    }
  }

  @SuppressWarnings("MissingPermission")
  private fun registerGpsProvider() {
    // Fallback listens to GPS only; NETWORK is used only as a last-known seed.
    val provider = LocationManager.GPS_PROVIDER
    try {
      if (locationManager.isProviderEnabled(provider)) {
        @Suppress("MissingPermission")
        locationManager.requestLocationUpdates(
            provider,
            REQUEST_INTERVAL_MS,
            REQUEST_MIN_DISTANCE_M,
            this,
            Looper.getMainLooper(),
        )
        Log.i(
            LOG_TAG,
            "event=listener_registered provider=$provider intervalMs=$REQUEST_INTERVAL_MS minDistanceM=$REQUEST_MIN_DISTANCE_M",
        )
      } else {
        Log.w(LOG_TAG, "event=listener_register_skipped provider=$provider reason=disabled")
      }
    } catch (error: SecurityException) {
      Log.w(LOG_TAG, "event=listener_register_failed provider=$provider reason=security")
    } catch (error: IllegalArgumentException) {
      Log.w(LOG_TAG, "event=listener_register_failed provider=$provider reason=${error.message}")
    }
  }

  private fun removeUpdates() {
    runCatching { locationManager.removeUpdates(this) }
    fusedCallback?.let { callback ->
      runCatching { fusedClient?.removeLocationUpdates(callback) }
    }
    fusedCallback = null
    fusedClient = null
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

  // Mirrors LocationHistoryStore.ts; change both together.
  private data class StationaryAccumulator(
      val source: String,
      var anchorTimestamp: Long,
      var count: Int,
      var sumWeight: Double,
      var sumWeightLat: Double,
      var sumWeightLon: Double,
      var sumWeightAlt: Double,
      var rawAccuracy: Double,
      var bestAccuracy: Double,
  )

  private data class MeanFix(
      val lat: Double,
      val lon: Double,
      val alt: Double,
      val accuracy: Double,
  )

  private var stationaryAccumulator: StationaryAccumulator? = null

  private fun accumWeight(accuracy: Double): Double {
    val a = max(accuracy, STATIONARY_ACCURACY_FLOOR_M)
    return 1.0 / (a * a)
  }

  private fun seedAccumulator(marker: JSONObject): StationaryAccumulator {
    val acc = marker.optDouble("accuracy", 0.0)
    val w = accumWeight(acc)
    return StationaryAccumulator(
        source = marker.optString("source"),
        anchorTimestamp = marker.optLong("timestamp", 0L),
        count = 1,
        sumWeight = w,
        sumWeightLat = w * marker.optDouble("lat", 0.0),
        sumWeightLon = w * marker.optDouble("lon", 0.0),
        sumWeightAlt = w * marker.optDouble("alt", 0.0),
        rawAccuracy = acc,
        bestAccuracy = if (acc > 0) acc else STATIONARY_ACCURACY_FLOOR_M,
    )
  }

  private fun foldAccumulator(acc: StationaryAccumulator, fix: JSONObject) {
    val a = fix.optDouble("accuracy", 0.0)
    val w = accumWeight(a)
    acc.count += 1
    acc.sumWeight += w
    acc.sumWeightLat += w * fix.optDouble("lat", 0.0)
    acc.sumWeightLon += w * fix.optDouble("lon", 0.0)
    acc.sumWeightAlt += w * fix.optDouble("alt", 0.0)
    if (a > acc.rawAccuracy) acc.rawAccuracy = a
    if (a > 0 && a < acc.bestAccuracy) acc.bestAccuracy = a
  }

  private fun accumulatedMean(acc: StationaryAccumulator): MeanFix {
    val n = min(acc.count, STATIONARY_AVG_SAMPLE_CAP)
    return MeanFix(
        lat = acc.sumWeightLat / acc.sumWeight,
        lon = acc.sumWeightLon / acc.sumWeight,
        alt = acc.sumWeightAlt / acc.sumWeight,
        accuracy = max(acc.bestAccuracy / sqrt(n.toDouble()), STATIONARY_ACCURACY_FLOOR_M),
    )
  }

  private fun withinMotionRadius(
      refLat: Double,
      refLon: Double,
      refRawAccuracy: Double,
      sample: JSONObject,
  ): Boolean {
    val thresholdM = STATIONARY_MOTION_K *
        max(max(refRawAccuracy, sample.optDouble("accuracy", 0.0)), STATIONARY_ACCURACY_FLOOR_M)
    return distanceMetersLL(
        refLat, refLon, sample.optDouble("lat", 0.0), sample.optDouble("lon", 0.0),
    ) <= thresholdM
  }

  private fun appendFix(fix: JSONObject): Boolean {
    val timestamp = fix.optLong("timestamp", 0L)
    if (timestamp <= 0) return false
    val source = fix.optString("source", "device")
    val bucketId = timestamp / DAY_MS

    val previousMarker = findPreviousMarker(bucketId, source)

    val acc = stationaryAccumulator
    val accMatches = acc != null && previousMarker != null &&
        acc.source == source && acc.anchorTimestamp == previousMarker.optLong("timestamp", 0L)

    var refLat = 0.0
    var refLon = 0.0
    var refRawAccuracy = 0.0
    if (accMatches) {
      val centroid = accumulatedMean(acc!!)
      refLat = centroid.lat
      refLon = centroid.lon
      refRawAccuracy = acc.rawAccuracy
    } else if (previousMarker != null) {
      refLat = previousMarker.optDouble("lat", 0.0)
      refLon = previousMarker.optDouble("lon", 0.0)
      refRawAccuracy = previousMarker.optDouble("accuracy", 0.0)
    }

    if (previousMarker != null && withinMotionRadius(refLat, refLon, refRawAccuracy, fix)) {
      val activeAcc = if (accMatches) acc!! else seedAccumulator(previousMarker).also {
        stationaryAccumulator = it
      }
      foldAccumulator(activeAcc, fix)
      val mean = accumulatedMean(activeAcc)
      val previousTimestamp = previousMarker.optLong("timestamp", 0L)

      if (timestamp - previousTimestamp >= STATIONARY_HEARTBEAT_MS) {
        // Heartbeat preserves temporal coverage while keeping the averaged fix.
        val marker = JSONObject().apply {
          put("lat", mean.lat)
          put("lon", mean.lon)
          put("alt", mean.alt)
          put("accuracy", mean.accuracy)
          put("timestamp", timestamp)
          put("source", source)
          put("provider", fix.opt("provider"))
        }
        val bucket = buckets.getOrPut(bucketId) { mutableListOf() }
        bucket.add(marker)
        dirtyBuckets.add(bucketId)
        activeAcc.anchorTimestamp = timestamp
        enforceCaps()
        return true
      }

      previousMarker.put("lat", mean.lat)
      previousMarker.put("lon", mean.lon)
      previousMarker.put("alt", mean.alt)
      previousMarker.put("accuracy", mean.accuracy)
      previousMarker.put("provider", fix.opt("provider"))
      dirtyBuckets.add(previousMarker.optLong("timestamp") / DAY_MS)
      return false
    }

    stationaryAccumulator = null
    val bucket = buckets.getOrPut(bucketId) { mutableListOf() }
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
      dirtyBuckets.add(id)
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
      val summary = summarizeBuckets()
      Log.i(
          LOG_TAG,
          "event=buckets_loaded buckets=${buckets.size} samples=${summary.samples} oldest=${summary.oldest} newest=${summary.newest}",
      )
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

    val dirtyCount = dirtyBuckets.size
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
    val summary = summarizeBuckets()
    Log.i(
        LOG_TAG,
        "event=flush buckets=${buckets.size} dirty=$dirtyCount samples=${summary.samples} oldest=${summary.oldest} newest=${summary.newest}",
    )
  }

  private fun bucketKey(id: Long): String = "$BUCKET_KEY_PREFIX$id"

  private fun summarizeBuckets(): BucketSummary {
    var samples = 0
    var oldest = Long.MAX_VALUE
    var newest = 0L
    for (list in buckets.values) {
      for (fix in list) {
        val timestamp = fix.optLong("timestamp", 0L)
        if (timestamp <= 0L) continue
        samples += 1
        if (timestamp < oldest) oldest = timestamp
        if (timestamp > newest) newest = timestamp
      }
    }
    return BucketSummary(
        samples = samples,
        oldest = if (oldest == Long.MAX_VALUE) 0L else oldest,
        newest = newest,
    )
  }

  private fun distanceMetersLL(latA: Double, lonA: Double, latB: Double, lonB: Double): Double {
    val radiusM = 6_371_000.0
    val lat1 = Math.toRadians(latA)
    val lat2 = Math.toRadians(latB)
    val deltaLat = Math.toRadians(latB - latA)
    val deltaLon = Math.toRadians(lonB - lonA)
    val sinLat = sin(deltaLat / 2)
    val sinLon = sin(deltaLon / 2)
    val h = sinLat * sinLat + cos(lat1) * cos(lat2) * sinLon * sinLon
    return 2 * radiusM * atan2(sqrt(h), sqrt(1 - h))
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

    // Request temporal coverage even while stationary. The accumulator below
    // coalesces these into one averaged marker per minute, so a fixed/home
    // receiver remains enrichable without growing history every 20 seconds.
    private const val REQUEST_INTERVAL_MS = 20_000L
    private const val REQUEST_MIN_DISTANCE_M = 0f
    // Fused-only batch window; keep priority high for enrichment quality.
    private const val REQUEST_MAX_BATCH_DELAY_MS = 60_000L
    private val REQUEST_PRIORITY = Priority.PRIORITY_HIGH_ACCURACY
    private const val STATIONARY_HEARTBEAT_MS = 60_000L
    private const val PERSIST_DEBOUNCE_MS = 30_000L
    private const val MAX_ACCURACY_M = 5_000.0

    // Stationary averaging parameters mirror LocationHistoryStore.ts.
    private const val STATIONARY_ACCURACY_FLOOR_M = 2.5
    private const val STATIONARY_AVG_SAMPLE_CAP = 16
    private const val STATIONARY_MOTION_K = 2.5

    private const val INDEX_KEY = "@spectre/location-history-v2/index"
    private const val BUCKET_KEY_PREFIX = "@spectre/location-history-v2/day/"

    fun start(context: Context) {
      val intent = Intent(context, SpectreLocationService::class.java)
      runCatching { context.startService(intent) }
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
