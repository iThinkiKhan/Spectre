package com.spectre.companion

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.util.Base64
import android.util.Log
import android.os.Handler
import android.os.Looper
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableMap
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.EOFException
import java.net.InetSocketAddress
import java.net.Socket
import java.net.ServerSocket
import java.io.DataInputStream
import java.io.DataOutputStream
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.security.SecureRandom
import java.time.Instant
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Native durable queue + minimal MQTT 3.1.1 QoS1 relay.
 *
 * Records enter this queue before Spectre receives its offload ACK.  SQLite
 * uniqueness makes BLE retries idempotent.  MQTT acknowledgements are written
 * back transactionally, providing at-least-once delivery across process or
 * cellular interruptions without requiring a WebSocket listener on the home
 * broker.
 */
class SpectreRelayModule(
    reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext) {
  private val db = RelayDb(reactContext.applicationContext)
  private val executor = Executors.newSingleThreadExecutor()
  private val relayRunning = AtomicBoolean(false)
  private val bulkExecutor = Executors.newSingleThreadExecutor()
  private val mainHandler = Handler(Looper.getMainLooper())
  private var bulkNetworkCallback: ConnectivityManager.NetworkCallback? = null
  private var bulkServer: ServerSocket? = null
  @Volatile private var bulkPhase = "idle"
  @Volatile private var bulkError = ""
  @Volatile private var bulkCopied = 0
  @Volatile private var bulkBytes = 0L
  @Volatile private var bulkStartedAt = 0L

  private data class BulkRecord(
      val sessionId: String,
      val eventId: Long,
      val lane: Int,
      val topic: String,
      val payload: ByteArray,
  )

  private data class NativeLocationFix(
      val lat: Double,
      val lon: Double,
      val alt: Double,
      val accuracy: Double,
      val timestamp: Long,
  )

  override fun getName(): String = "SpectreRelay"

  @ReactMethod
  fun startBulkReceiver(promise: Promise) {
    mainHandler.post {
      try {
        if (bulkNetworkCallback != null || bulkPhase == "starting" || bulkPhase == "receiving") {
          promise.reject("E_BULK_ACTIVE", "A bulk receiver is already active")
          return@post
        }
        val server = ServerSocket(0).apply {
          reuseAddress = true
          soTimeout = BULK_ACCEPT_TIMEOUT_MS
        }
        val token = ByteArray(BULK_TOKEN_BYTES).also { SecureRandom().nextBytes(it) }
        bulkServer = server
        bulkPhase = "starting"
        bulkError = ""
        bulkCopied = 0
        bulkBytes = 0
        bulkStartedAt = System.currentTimeMillis()
        // Spectre hosts the private WPA2 AP. Samsung rewrites app-hosted
        // LocalOnlyHotspot requests into WPA2/WPA3 transition networks that
        // the S3 cannot authenticate against reliably. A specifier keeps the
        // field link local (no default-route takeover) and Android remembers
        // approval for the same app/network on supported releases.
        val random = ByteArray(12).also { SecureRandom().nextBytes(it) }
        // Keep the AP identity stable so Android's one-time user approval is
        // reusable in the field. The WPA2 passphrase and TCP bearer token are
        // still freshly generated for every transfer.
        val ssid = BULK_FIELD_SSID
        val password = Base64.encodeToString(random, Base64.NO_WRAP or Base64.URL_SAFE)
        val connectivity = reactApplicationContext.applicationContext
            .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(ssid)
            .setWpa2Passphrase(password)
            .build()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(specifier)
            .build()
        val callback = object : ConnectivityManager.NetworkCallback() {
          override fun onAvailable(network: Network) {
            Log.i(LOG_TAG, "bulk_field_network_available ssid=$ssid port=${server.localPort}")
          }

          override fun onUnavailable() {
            Log.e(LOG_TAG, "bulk_field_network_unavailable ssid=$ssid")
            stopBulkReceiverInternal("Spectre field network unavailable")
          }

          override fun onLost(network: Network) {
            Log.i(LOG_TAG, "bulk_field_network_lost ssid=$ssid phase=$bulkPhase")
            if (bulkPhase != "complete") stopBulkReceiverInternal("Spectre field network lost")
          }
        }
        bulkNetworkCallback = callback
        bulkPhase = "waiting"
        runBulkServer(server, token)
        // Resolving the endpoint first lets JavaScript deliver CMD 0x34 over
        // the still-live BLE session. requestNetwork can foreground Android's
        // approval sheet; starting it synchronously pauses React before
        // Spectre has a chance to create the AP the sheet is searching for.
        mainHandler.postDelayed({
          if (bulkNetworkCallback === callback) {
            try {
              connectivity.requestNetwork(request, callback)
              Log.i(LOG_TAG, "bulk_field_network_requested ssid=$ssid port=${server.localPort}")
            } catch (error: Exception) {
              Log.e(LOG_TAG, "bulk_field_network_request_failed", error)
              stopBulkReceiverInternal(error.message ?: "Spectre field network request failed")
            }
          }
        }, BULK_NETWORK_REQUEST_DELAY_MS)
        Log.i(LOG_TAG, "bulk_field_network_scheduled ssid=$ssid port=${server.localPort}")
        promise.resolve(Arguments.createMap().apply {
          putString("ssid", ssid)
          putString("password", password)
          putInt("port", server.localPort)
          putString("tokenBase64", Base64.encodeToString(token, Base64.NO_WRAP))
        })
      } catch (error: Exception) {
        stopBulkReceiverInternal(error.message ?: "Bulk receiver failed")
        promise.reject("E_BULK_START", error.message, error)
      }
    }
  }

  @ReactMethod
  fun getBulkStatus(promise: Promise) {
    promise.resolve(Arguments.createMap().apply {
      putString("phase", bulkPhase)
      putString("error", bulkError)
      putInt("copied", bulkCopied)
      putDouble("bytes", bulkBytes.toDouble())
      putDouble("elapsedMs", (if (bulkStartedAt > 0) System.currentTimeMillis() - bulkStartedAt else 0).toDouble())
    })
  }

  @ReactMethod
  fun stopBulkReceiver(promise: Promise) {
    mainHandler.post {
      stopBulkReceiverInternal("")
      promise.resolve(null)
    }
  }

  private fun runBulkServer(server: ServerSocket, token: ByteArray) {
    bulkExecutor.execute {
      try {
        server.accept().use { socket ->
          Log.i(LOG_TAG, "bulk_client_accepted remote=${socket.inetAddress.hostAddress}")
          // Report wire throughput, not the time Android spent waiting for a
          // one-time network approval or for Spectre to reboot into AP mode.
          bulkStartedAt = System.currentTimeMillis()
          socket.soTimeout = BULK_SOCKET_TIMEOUT_MS
          socket.tcpNoDelay = true
          val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 64 * 1024))
          val output = DataOutputStream(BufferedOutputStream(socket.getOutputStream(), 64 * 1024))
          if (input.readInt() != BULK_MAGIC) throw IllegalArgumentException("Bulk magic mismatch")
          val offeredToken = ByteArray(BULK_TOKEN_BYTES)
          input.readFully(offeredToken)
          if (!MessageDigest.isEqual(token, offeredToken)) throw SecurityException("Bulk token rejected")
          bulkPhase = "receiving"
          val locations = loadNativeLocationHistory()
          while (true) {
            val count = input.readUnsignedShort()
            if (count == 0) {
              output.writeInt(BULK_ACK_MAGIC)
              output.writeShort(0)
              output.writeShort(0)
              output.writeInt(0)
              output.flush()
              bulkPhase = "complete"
              break
            }
            if (count > BULK_BATCH_RECORDS_MAX) throw IllegalArgumentException("Bulk batch too large")
            val records = ArrayList<BulkRecord>(count)
            repeat(count) {
              val eventId = input.readInt().toLong() and 0xffffffffL
              val lane = input.readUnsignedByte()
              val sessionLen = input.readUnsignedByte()
              val topicLen = input.readUnsignedByte()
              input.readUnsignedByte() // reserved
              val bodyLen = input.readUnsignedShort()
              if (eventId == 0L || sessionLen !in 1..63 || topicLen !in 1..255 ||
                  bodyLen <= sessionLen + topicLen || bodyLen > MAX_PAYLOAD_BYTES + 320) {
                throw IllegalArgumentException("Invalid bulk record")
              }
              val body = ByteArray(bodyLen)
              input.readFully(body)
              val sessionId = String(body, 0, sessionLen, StandardCharsets.UTF_8)
              val topic = String(body, sessionLen, topicLen, StandardCharsets.UTF_8)
              val payloadOffset = sessionLen + topicLen
              val rawPayload = body.copyOfRange(payloadOffset, body.size)
              val payload = enrichBulkPayload(rawPayload, locations)
              records.add(BulkRecord(sessionId, eventId, lane, topic, payload))
            }
            persistBulkBatch(records)
            val lastEventId = records.last().eventId
            bulkCopied += records.size
            bulkBytes += records.sumOf { it.payload.size.toLong() }
            output.writeInt(BULK_ACK_MAGIC)
            output.writeShort(records.size)
            output.writeShort(0)
            output.writeInt(lastEventId.toInt())
            output.flush()
          }
        }
      } catch (error: Exception) {
        bulkError = error.message ?: error.javaClass.simpleName
        bulkPhase = "error"
      } finally {
        try { server.close() } catch (_: Exception) {}
      }
    }
  }

  private fun persistBulkBatch(records: List<BulkRecord>) {
    val writable = db.writableDatabase
    writable.beginTransaction()
    try {
      records.forEach { record -> upsertBulkRecord(writable, record) }
      writable.setTransactionSuccessful()
    } finally {
      writable.endTransaction()
    }
  }

  private fun upsertBulkRecord(writable: SQLiteDatabase, record: BulkRecord) {
    val digest = relayDigest(record.sessionId, record.eventId, record.topic, record.payload)
    val values = ContentValues().apply {
      put("session_id", record.sessionId); put("event_id", record.eventId)
      put("lane", record.lane); put("topic", record.topic); put("payload", record.payload)
      put("digest", digest); put("received_at", System.currentTimeMillis()); put("attempts", 0)
    }
    val rowId = writable.insertWithOnConflict(TABLE_QUEUE, null, values, SQLiteDatabase.CONFLICT_IGNORE)
    if (rowId != -1L) return
    var existingDigest: String? = null
    writable.query(TABLE_QUEUE, arrayOf("digest"), "session_id=? AND event_id=?",
        arrayOf(record.sessionId, record.eventId.toString()), null, null, null, "1").use { cursor ->
      if (cursor.moveToFirst()) existingDigest = cursor.getString(0)
    }
    if (existingDigest != digest) {
      values.putNull("published_at"); values.put("attempts", 0); values.putNull("last_error")
      if (writable.update(TABLE_QUEUE, values, "session_id=? AND event_id=?",
          arrayOf(record.sessionId, record.eventId.toString())) != 1) {
        throw IllegalStateException("Bulk relay revision was not committed")
      }
    }
  }

  private fun relayDigest(sessionId: String, eventId: Long, topic: String, payload: ByteArray) =
      sha256Hex(sessionId.toByteArray(StandardCharsets.UTF_8) + byteArrayOf(0) +
          eventId.toString().toByteArray(StandardCharsets.US_ASCII) + byteArrayOf(0) +
          topic.toByteArray(StandardCharsets.UTF_8) + byteArrayOf(0) + payload)

  private fun enrichBulkPayload(payload: ByteArray, locations: List<NativeLocationFix>): ByteArray {
    return try {
      val json = JSONObject(String(payload, StandardCharsets.UTF_8))
      if (json.has("lat") && json.has("lon")) return payload
      val timestamp = try { Instant.parse(json.optString("ts")).toEpochMilli() } catch (_: Exception) { 0L }
      val fix = findNativeLocationForEvent(timestamp, locations)
      if (fix != null) {
        json.put("lat", fix.lat); json.put("lon", fix.lon); json.put("alt", fix.alt)
        json.put("acc", fix.accuracy); json.put("gps_ts", fix.timestamp / 1000L)
        json.remove("enrich_no_data")
      } else if (timestamp > 0L) {
        json.put("enrich_no_data", true)
      }
      json.toString().toByteArray(StandardCharsets.UTF_8)
    } catch (_: Exception) { payload }
  }

  private fun loadNativeLocationHistory(): List<NativeLocationFix> {
    val prefs = reactApplicationContext.getSharedPreferences("spectre_companion_store", Context.MODE_PRIVATE)
    val indexRaw = prefs.getString(LOCATION_HISTORY_INDEX_KEY, null) ?: return emptyList()
    val cutoff = System.currentTimeMillis() - LOCATION_HISTORY_MAX_AGE_MS
    val samples = mutableListOf<NativeLocationFix>()
    try {
      val index = JSONArray(indexRaw)
      for (i in 0 until index.length()) {
        val id = index.optLong(i, Long.MIN_VALUE); if (id == Long.MIN_VALUE) continue
        val raw = prefs.getString("$LOCATION_HISTORY_BUCKET_PREFIX$id", null) ?: continue
        val bucket = JSONArray(raw)
        for (j in 0 until bucket.length()) {
          val fix = bucket.optJSONObject(j) ?: continue
          val ts = fix.optLong("timestamp", 0L); if (ts < cutoff) continue
          val lat = fix.optDouble("lat", Double.NaN); val lon = fix.optDouble("lon", Double.NaN)
          if (!lat.isNaN() && !lon.isNaN()) samples.add(NativeLocationFix(lat, lon,
              fix.optDouble("alt", 0.0), fix.optDouble("accuracy", 0.0).coerceAtLeast(0.0), ts))
        }
      }
    } catch (_: Exception) { return emptyList() }
    return samples.sortedBy { it.timestamp }
  }

  private fun findNativeLocationForEvent(eventMs: Long, locations: List<NativeLocationFix>): NativeLocationFix? {
    if (eventMs <= 0L) return null
    var nearest: NativeLocationFix? = null; var drift = Long.MAX_VALUE
    for (fix in locations) {
      val d = kotlin.math.abs(fix.timestamp - eventMs)
      if (d < drift) { nearest = fix; drift = d }
      if (fix.timestamp > eventMs && d > drift) break
    }
    return nearest?.takeIf { drift <= LOCATION_MATCH_MAX_DRIFT_MS }
  }

  private fun stopBulkReceiverInternal(error: String) {
    try { bulkServer?.close() } catch (_: Exception) {}
    bulkServer = null
    val callback = bulkNetworkCallback
    bulkNetworkCallback = null
    if (callback != null) {
      try {
        val connectivity = reactApplicationContext.applicationContext
            .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        connectivity.unregisterNetworkCallback(callback)
      } catch (_: Exception) {}
    }
    if (error.isNotBlank()) { bulkError = error; bulkPhase = "error" }
    else if (bulkPhase != "complete") bulkPhase = "idle"
  }

  @ReactMethod
  fun enqueueRecord(record: ReadableMap, promise: Promise) {
    executor.execute {
      try {
        val sessionId = requiredString(record, "sessionId", 63)
        val topic = requiredString(record, "topic", 255)
        val payloadBase64 = requiredString(record, "payloadBase64", 16_384)
        val payload = Base64.decode(payloadBase64, Base64.DEFAULT)
        val eventId = record.getDouble("eventId").toLong()
        val lane = if (record.hasKey("lane")) record.getInt("lane") else 0xff
        if (eventId <= 0L || payload.isEmpty() || payload.size > MAX_PAYLOAD_BYTES) {
          throw IllegalArgumentException("Invalid relay record")
        }

        val digest = sha256Hex(
            sessionId.toByteArray(StandardCharsets.UTF_8) +
                byteArrayOf(0) +
                eventId.toString().toByteArray(StandardCharsets.US_ASCII) +
                byteArrayOf(0) +
                topic.toByteArray(StandardCharsets.UTF_8) +
                byteArrayOf(0) + payload,
        )
        val values = ContentValues().apply {
          put("session_id", sessionId)
          put("event_id", eventId)
          put("lane", lane)
          put("topic", topic)
          put("payload", payload)
          put("digest", digest)
          put("received_at", System.currentTimeMillis())
          put("attempts", 0)
        }
        val writable = db.writableDatabase
        var committedNewVersion = false
        writable.beginTransaction()
        try {
          val rowId = writable.insertWithOnConflict(
              TABLE_QUEUE,
              null,
              values,
              SQLiteDatabase.CONFLICT_IGNORE,
          )
          committedNewVersion = rowId != -1L
          if (!committedNewVersion) {
            var existingDigest: String? = null
            writable.query(
                TABLE_QUEUE,
                arrayOf("digest"),
                "session_id=? AND event_id=?",
                arrayOf(sessionId, eventId.toString()),
                null,
                null,
                null,
                "1",
            ).use { cursor ->
              if (cursor.moveToFirst()) existingDigest = cursor.getString(0)
            }

            if (existingDigest != digest) {
              // The same capture can legitimately return after Spectre joins a
              // later phone-GPS enrichment delta. Persist and re-publish that
              // corrected payload; byte-identical BLE retries stay no-ops.
              values.putNull("published_at")
              values.put("attempts", 0)
              values.putNull("last_error")
              val updated = writable.update(
                  TABLE_QUEUE,
                  values,
                  "session_id=? AND event_id=?",
                  arrayOf(sessionId, eventId.toString()),
              )
              if (updated != 1) {
                throw IllegalStateException("Relay event revision was not committed")
              }
              committedNewVersion = true
            }
          }
          writable.setTransactionSuccessful()
        } finally {
          writable.endTransaction()
        }
        val result = Arguments.createMap().apply {
          putBoolean("inserted", committedNewVersion)
          putString("digest", digest)
        }
        promise.resolve(result)
      } catch (error: Exception) {
        promise.reject("E_RELAY_ENQUEUE", error.message, error)
      }
    }
  }

  @ReactMethod
  fun getStatus(promise: Promise) {
    executor.execute {
      try {
        promise.resolve(statusMap())
      } catch (error: Exception) {
        promise.reject("E_RELAY_STATUS", error.message, error)
      }
    }
  }

  /**
   * Read a bounded, newest-first view of the durable archive for local field
   * localization. Published rows intentionally remain available: the phone
   * map must keep working when WireGuard or the home stack is unavailable.
   */
  @ReactMethod
  fun getRecentRecords(limit: Int, promise: Promise) {
    executor.execute {
      try {
        val boundedLimit = limit.coerceIn(1, MAX_ARCHIVE_RECORDS)
        val rows = Arguments.createArray()
        db.readableDatabase.query(
            TABLE_QUEUE,
            arrayOf(
                "session_id",
                "event_id",
                "lane",
                "topic",
                "payload",
                "received_at",
                "published_at",
            ),
            null,
            null,
            null,
            null,
            "_id DESC",
            boundedLimit.toString(),
        ).use { cursor ->
          while (cursor.moveToNext()) {
            val payloadText = String(cursor.getBlob(4), StandardCharsets.UTF_8)
            rows.pushMap(Arguments.createMap().apply {
              putString("sessionId", cursor.getString(0))
              putDouble("eventId", cursor.getLong(1).toDouble())
              putInt("lane", cursor.getInt(2))
              putString("topic", cursor.getString(3))
              putString(
                  "payload",
                  payloadText,
              )
              putDouble("receivedAt", cursor.getLong(5).toDouble())
              if (cursor.isNull(6)) putNull("publishedAt")
              else putDouble("publishedAt", cursor.getLong(6).toDouble())
            })
          }
        }
        promise.resolve(rows)
      } catch (error: Exception) {
        promise.reject("E_RELAY_ARCHIVE", error.message, error)
      }
    }
  }

  @ReactMethod
  fun getNetworkStatus(promise: Promise) {
    executor.execute {
      try {
        val connectivity = reactApplicationContext.getSystemService(
            Context.CONNECTIVITY_SERVICE,
        ) as ConnectivityManager
        var vpnActive = false
        var vpnValidated = false
        var vpnInterface = ""
        var cellularAvailable = false

        for (network in connectivity.allNetworks) {
          val capabilities = connectivity.getNetworkCapabilities(network) ?: continue
          if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) &&
              capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) {
            cellularAvailable = true
          }
          if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN) &&
              capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) {
            vpnActive = true
            vpnValidated = vpnValidated || capabilities.hasCapability(
                NetworkCapabilities.NET_CAPABILITY_VALIDATED,
            )
            if (vpnInterface.isEmpty()) {
              vpnInterface = connectivity.getLinkProperties(network)?.interfaceName.orEmpty()
            }
          }
        }

        promise.resolve(Arguments.createMap().apply {
          putBoolean("vpnActive", vpnActive)
          putBoolean("vpnValidated", vpnValidated)
          putString("vpnInterface", vpnInterface)
          putBoolean("cellularAvailable", cellularAvailable)
        })
      } catch (error: Exception) {
        promise.reject("E_RELAY_NETWORK", error.message, error)
      }
    }
  }

  @ReactMethod
  fun relayPending(options: ReadableMap?, promise: Promise) {
    if (!relayRunning.compareAndSet(false, true)) {
      promise.reject("E_RELAY_BUSY", "A relay pass is already running")
      return
    }
    executor.execute {
      try {
        val host = options?.takeIf { it.hasKey("host") }?.getString("host")
            ?.trim()?.takeIf { it.isNotEmpty() } ?: DEFAULT_HOST
        val port = options?.takeIf { it.hasKey("port") }?.getInt("port")
            ?: DEFAULT_PORT
        if (port !in 1..65535) throw IllegalArgumentException("Invalid MQTT port")

        val pending = loadPending(MAX_RECORDS_PER_PASS)
        if (pending.isEmpty()) {
          promise.resolve(statusMap(publishedThisPass = 0, endpoint = "$host:$port"))
          return@execute
        }

        var published = 0
        MqttQos1Client(host, port).use { mqtt ->
          mqtt.connect()
          for (record in pending) {
            try {
              mqtt.publish(record.topic, record.payload)
              markPublished(record.rowId)
              published += 1
            } catch (error: Exception) {
              markFailure(record.rowId, error.message ?: error.javaClass.simpleName)
              throw error
            }
          }
        }
        promise.resolve(statusMap(publishedThisPass = published, endpoint = "$host:$port"))
      } catch (error: Exception) {
        promise.reject("E_RELAY_PUBLISH", error.message, error)
      } finally {
        relayRunning.set(false)
      }
    }
  }

  override fun invalidate() {
    stopBulkReceiverInternal("")
    bulkExecutor.shutdownNow()
    super.invalidate()
    executor.shutdown()
    db.close()
  }

  private fun statusMap(
      publishedThisPass: Int = 0,
      endpoint: String = "$DEFAULT_HOST:$DEFAULT_PORT",
  ) = Arguments.createMap().apply {
    val sql = "SELECT " +
        "SUM(CASE WHEN published_at IS NULL THEN 1 ELSE 0 END), " +
        "SUM(CASE WHEN published_at IS NOT NULL THEN 1 ELSE 0 END), " +
        "COALESCE(SUM(CASE WHEN published_at IS NULL THEN length(payload) ELSE 0 END), 0) " +
        "FROM $TABLE_QUEUE"
    db.readableDatabase.rawQuery(sql, null).use { cursor ->
      cursor.moveToFirst()
      putInt("pending", cursor.getInt(0))
      putInt("published", cursor.getInt(1))
      putDouble("pendingBytes", cursor.getLong(2).toDouble())
    }
    putBoolean("running", relayRunning.get())
    putInt("publishedThisPass", publishedThisPass)
    putString("endpoint", endpoint)
  }

  private fun loadPending(limit: Int): List<RelayRecord> {
    val records = mutableListOf<RelayRecord>()
    db.readableDatabase.query(
        TABLE_QUEUE,
        arrayOf("_id", "topic", "payload"),
        "published_at IS NULL",
        null,
        null,
        null,
        "_id ASC",
        limit.toString(),
    ).use { cursor ->
      while (cursor.moveToNext()) {
        records += RelayRecord(
            rowId = cursor.getLong(0),
            topic = cursor.getString(1),
            payload = cursor.getBlob(2),
        )
      }
    }
    return records
  }

  private fun markPublished(rowId: Long) {
    val values = ContentValues().apply {
      put("published_at", System.currentTimeMillis())
      putNull("last_error")
    }
    db.writableDatabase.update(TABLE_QUEUE, values, "_id=?", arrayOf(rowId.toString()))
  }

  private fun markFailure(rowId: Long, message: String) {
    db.writableDatabase.execSQL(
        "UPDATE $TABLE_QUEUE SET attempts=attempts+1, last_error=? WHERE _id=?",
        arrayOf<Any>(message.take(240), rowId),
    )
  }

  private fun requiredString(map: ReadableMap, key: String, maxChars: Int): String {
    val value = map.takeIf { it.hasKey(key) }?.getString(key)?.trim().orEmpty()
    if (value.isEmpty() || value.length > maxChars) {
      throw IllegalArgumentException("Invalid $key")
    }
    return value
  }

  private data class RelayRecord(
      val rowId: Long,
      val topic: String,
      val payload: ByteArray,
  )

  private class RelayDb(context: Context) : SQLiteOpenHelper(
      context,
      "spectre_field_relay.db",
      null,
      DB_VERSION,
  ) {
    override fun onConfigure(db: SQLiteDatabase) {
      super.onConfigure(db)
      db.enableWriteAheadLogging()
      db.setForeignKeyConstraintsEnabled(true)
    }

    override fun onCreate(db: SQLiteDatabase) {
      db.execSQL(
          "CREATE TABLE $TABLE_QUEUE (" +
              "_id INTEGER PRIMARY KEY AUTOINCREMENT," +
              "session_id TEXT NOT NULL," +
              "event_id INTEGER NOT NULL," +
              "lane INTEGER NOT NULL," +
              "topic TEXT NOT NULL," +
              "payload BLOB NOT NULL," +
              "digest TEXT NOT NULL UNIQUE," +
              "received_at INTEGER NOT NULL," +
              "published_at INTEGER," +
              "attempts INTEGER NOT NULL DEFAULT 0," +
              "last_error TEXT" +
              ")",
      )
      db.execSQL(
          "CREATE INDEX relay_pending_idx ON $TABLE_QUEUE(published_at, _id)",
      )
      db.execSQL(
          "CREATE UNIQUE INDEX relay_event_idx ON $TABLE_QUEUE(session_id, event_id)",
      )
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
      if (oldVersion < 2) {
        // Event identity—not payload equality—is the durable idempotency key.
        // Two captures can legitimately publish identical JSON, while a BLE
        // retry of the same session/event must remain a no-op.
        db.execSQL(
            "CREATE UNIQUE INDEX IF NOT EXISTS relay_event_idx " +
                "ON $TABLE_QUEUE(session_id, event_id)",
        )
      }
    }
  }

  private class MqttQos1Client(
      private val host: String,
      private val port: Int,
  ) : AutoCloseable {
    private val socket = Socket()
    private lateinit var input: BufferedInputStream
    private lateinit var output: BufferedOutputStream
    private var nextPacketId = 1
    private var connected = false

    fun connect() {
      socket.connect(InetSocketAddress(host, port), SOCKET_TIMEOUT_MS)
      socket.soTimeout = SOCKET_TIMEOUT_MS
      socket.tcpNoDelay = true
      input = BufferedInputStream(socket.getInputStream())
      output = BufferedOutputStream(socket.getOutputStream())

      val variable = ByteArrayBuilder()
      variable.writeUtf8("MQTT")
      variable.writeByte(4) // MQTT 3.1.1
      variable.writeByte(0x02) // clean session, no credentials
      variable.writeU16(30) // keepalive seconds
      variable.writeUtf8("spectre-phone-${System.currentTimeMillis().toString(16)}")
      writePacket(0x10, variable.toByteArray())

      val packet = readPacket()
      if (packet.type != 0x20 || packet.body.size != 2 || packet.body[1].toInt() != 0) {
        val code = packet.body.getOrNull(1)?.toInt()?.and(0xff) ?: -1
        throw IllegalStateException("MQTT CONNACK rejected code=$code")
      }
      connected = true
    }

    fun publish(topic: String, payload: ByteArray) {
      if (!connected) throw IllegalStateException("MQTT client not connected")
      if (payload.isEmpty() || payload.size > MAX_PAYLOAD_BYTES) {
        throw IllegalArgumentException("Invalid MQTT payload")
      }
      val packetId = allocatePacketId()
      val body = ByteArrayBuilder()
      body.writeUtf8(topic)
      body.writeU16(packetId)
      body.write(payload)
      writePacket(0x32, body.toByteArray()) // PUBLISH QoS1

      while (true) {
        val packet = readPacket()
        if (packet.type == 0x40 && packet.body.size == 2) {
          val ackId = ((packet.body[0].toInt() and 0xff) shl 8) or
              (packet.body[1].toInt() and 0xff)
          if (ackId == packetId) return
        }
      }
    }

    override fun close() {
      try {
        if (connected) writePacket(0xe0, ByteArray(0))
      } catch (_: Exception) {
      }
      connected = false
      try { socket.close() } catch (_: Exception) {}
    }

    private fun allocatePacketId(): Int {
      val id = nextPacketId
      nextPacketId += 1
      if (nextPacketId > 0xffff) nextPacketId = 1
      return id
    }

    private fun writePacket(header: Int, body: ByteArray) {
      output.write(header)
      var remaining = body.size
      do {
        var encoded = remaining % 128
        remaining /= 128
        if (remaining > 0) encoded = encoded or 0x80
        output.write(encoded)
      } while (remaining > 0)
      output.write(body)
      output.flush()
    }

    private fun readPacket(): MqttPacket {
      val type = input.read()
      if (type < 0) throw EOFException("MQTT connection closed")
      var multiplier = 1
      var remaining = 0
      var count = 0
      do {
        val value = input.read()
        if (value < 0) throw EOFException("MQTT remaining length truncated")
        remaining += (value and 0x7f) * multiplier
        multiplier *= 128
        count += 1
        if (count > 4) throw IllegalStateException("Malformed MQTT remaining length")
      } while ((value and 0x80) != 0)
      val body = ByteArray(remaining)
      var offset = 0
      while (offset < remaining) {
        val read = input.read(body, offset, remaining - offset)
        if (read < 0) throw EOFException("MQTT packet truncated")
        offset += read
      }
      return MqttPacket(type and 0xf0, body)
    }

    private data class MqttPacket(val type: Int, val body: ByteArray)
  }

  private class ByteArrayBuilder {
    private val bytes = ArrayList<Byte>()

    fun writeByte(value: Int) {
      bytes.add((value and 0xff).toByte())
    }

    fun writeU16(value: Int) {
      writeByte(value ushr 8)
      writeByte(value)
    }

    fun writeUtf8(value: String) {
      val encoded = value.toByteArray(StandardCharsets.UTF_8)
      if (encoded.size > 0xffff) throw IllegalArgumentException("MQTT string too long")
      writeU16(encoded.size)
      write(encoded)
    }

    fun write(value: ByteArray) {
      value.forEach { bytes.add(it) }
    }

    fun toByteArray(): ByteArray = ByteArray(bytes.size) { bytes[it] }
  }

  companion object {
    private const val BULK_FIELD_SSID = "Spectre-FieldLink"
    private const val DB_VERSION = 2
    private const val TABLE_QUEUE = "relay_queue"
    private const val DEFAULT_HOST = "192.168.0.11"
    private const val DEFAULT_PORT = 1883
    private const val SOCKET_TIMEOUT_MS = 8_000
    private const val MAX_PAYLOAD_BYTES = 4_096
    private const val MAX_RECORDS_PER_PASS = 2_000
    private const val MAX_ARCHIVE_RECORDS = 5_000
    private const val BULK_MAGIC = 0x53504231
    private const val BULK_ACK_MAGIC = 0x41434b31
    private const val BULK_TOKEN_BYTES = 32
    private const val LOG_TAG = "SpectreRelay"
    private const val BULK_BATCH_RECORDS_MAX = 128
    private const val BULK_ACCEPT_TIMEOUT_MS = 120_000
    private const val BULK_SOCKET_TIMEOUT_MS = 30_000
    private const val BULK_NETWORK_REQUEST_DELAY_MS = 1_500L
    private const val LOCATION_HISTORY_INDEX_KEY = "@spectre/location-history-v2/index"
    private const val LOCATION_HISTORY_BUCKET_PREFIX = "@spectre/location-history-v2/day/"
    private const val LOCATION_HISTORY_MAX_AGE_MS = 30L * 24 * 60 * 60 * 1000
    private const val LOCATION_MATCH_MAX_DRIFT_MS = 5L * 60 * 1000

    private fun sha256Hex(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256")
            .digest(bytes)
            .joinToString("") { "%02x".format(it) }
  }
}
