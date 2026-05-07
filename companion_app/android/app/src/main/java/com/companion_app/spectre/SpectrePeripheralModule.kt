package com.companion_app.spectre

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Base64
import android.util.Log
import androidx.core.content.ContextCompat
import com.companion_app.BuildConfig
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
import java.nio.charset.StandardCharsets
import java.util.ArrayDeque
import java.util.LinkedHashMap
import java.util.UUID

class SpectrePeripheralModule(
  reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext) {

  companion object {
    private const val LOG_TAG = "SpectrePeripheral"
    private val PHONE_SERVICE_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001")
    private val PHONE_GPS_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002")
    private val PHONE_CONTROL_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003")
    private val PHONE_METADATA_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004")
    private val PHONE_EVENT_BATCH_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0005")
    private val PHONE_ENRICHMENT_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0006")
    private val PHONE_AUTH_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0007")
    private val PHONE_STORAGE_UUID =
      UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0008")
    private val CCCD_UUID =
      UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private const val DEVICE_NAME = "SpectrePhone"
    // Short adapter name used by SHORT_NAME diagnostic mode.  Six 7-bit
    // ASCII chars => 6 bytes payload (+1 length +1 type) which leaves
    // plenty of room inside the 31-byte legacy AD PDU even with flags.
    private const val DEVICE_NAME_SHORT = "SPHONE"
    private const val MODE_NAME_ONLY = "nameOnly"
    private const val MODE_UUID_ONLY = "uuidOnly"
    private const val MODE_SERVICE = "service"
    private const val MODE_SHORT_NAME = "shortName"

    // Cadence of the advertiser watchdog log emitted while the GATT
    // server is running.  Short enough to make stalls obvious in the
    // logcat stream during a 30s ESP32 diagnostic window.
    private const val ADV_WATCHDOG_PERIOD_MS = 3000L
    private const val BEACON_REFRESH_PERIOD_MS = 5000L
    private const val CONNECTED_BEACON_REFRESH_PERIOD_MS = 12000L

    // How long to wait after startAdvertising() before the watchdog is
    // allowed to declare the callback missing and restart the advertiser.
    // The Android BLE stack can take a few hundred ms to deliver
    // onStartSuccess; without this window the first watchdog tick (which
    // fires immediately) races the callback and triggers a spurious
    // restart.
    private const val ADVERTISE_START_GRACE_MS = 1500L

    // Android does not fragment a large notification in a way NimBLE can
    // reassemble for this protocol, so send deterministic ATT-sized chunks.
    private const val DEFAULT_NOTIFICATION_PAYLOAD_BYTES = 20
    private const val MAX_NOTIFICATION_PAYLOAD_BYTES = 180
    private const val PHONE_STORAGE_FRAME_SIZE = 68
    private const val EVENT_BATCH_RECORD_SIZE = 10
    private const val PHONE_EVENT_BATCH_MAX_RECORDS = 512
    private const val PHONE_EVENT_BATCH_MAX_BYTES =
      PHONE_EVENT_BATCH_MAX_RECORDS * EVENT_BATCH_RECORD_SIZE
    private const val GATT_INSUFFICIENT_AUTHORIZATION_STATUS = 8
  }

  private data class PendingNotification(
    val device: BluetoothDevice,
    val characteristic: BluetoothGattCharacteristic,
    val value: ByteArray,
    val label: String,
    val index: Int,
    val total: Int,
  )

  private enum class AdvertisePayloadMode(val configValue: String) {
    NAME_ONLY("nameOnly"),
    UUID_ONLY("uuidOnly"),
    SERVICE_UUID_WITH_NAME_SCAN_RESPONSE("service"),
    SHORT_NAME("shortName"),
  }

  private val connectedDevices = LinkedHashMap<String, BluetoothDevice>()
  private var advertiser: BluetoothLeAdvertiser? = null
  private var advertiseCallback: AdvertiseCallback? = null
  private var gattServer: BluetoothGattServer? = null
  private var ownerDeviceAddress: String? = null

  private var gpsCharacteristic: BluetoothGattCharacteristic? = null
  private var controlCharacteristic: BluetoothGattCharacteristic? = null
  private var metadataCharacteristic: BluetoothGattCharacteristic? = null
  private var eventBatchCharacteristic: BluetoothGattCharacteristic? = null
  private var enrichmentCharacteristic: BluetoothGattCharacteristic? = null
  private var authCharacteristic: BluetoothGattCharacteristic? = null
  private var storageCharacteristic: BluetoothGattCharacteristic? = null
  private val secureSession = SpectreSecureSession(::emitLog)

  private var gpsBytes = ByteArray(0)
  private var controlBytes = ByteArray(0)
  private var metadataBytes = ByteArray(0)
  private var eventBatchBytes = ByteArray(0)
  private var enrichmentBytes = ByteArray(0)
  private var storageBytes = ByteArray(0)
  private var advertising = false
  private var advertiseMode = AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
  private var preferredAdvertiseMode = AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
  private var lastError: String? = null
  private var lastConnectedAtMs = 0L
  private var lastDisconnectedAtMs = 0L
  private var lastConnectedPeerLabel: String? = null
  private var lastDisconnectedPeerLabel: String? = null
  private var lastBatchReceivedAtMs = 0L
  private var lastBatchPeerLabel: String? = null
  private var lastBatchBytes = 0
  private var lastBatchRecords = 0
  private var lastStorageReceivedAtMs = 0L
  private var lastStoragePeerLabel: String? = null
  private var totalBatchesReceived = 0
  private var totalBatchBytes = 0
  private var totalBatchRecords = 0
  private var totalAdvertiseRestarts = 0
  private val negotiatedMtuByAddress = mutableMapOf<String, Int>()
  private val preparedAuthByAddress = mutableMapOf<String, ByteArray>()
  private val preparedEventBatchByAddress = mutableMapOf<String, ByteArray>()
  private val preparedStorageByAddress = mutableMapOf<String, ByteArray>()
  private val readEnvelopeByPeerAndCharacteristic = mutableMapOf<String, ByteArray>()
  private val notificationQueue = ArrayDeque<PendingNotification>()
  private var notificationInFlight: PendingNotification? = null

  // Watchdog state — keeps logcat showing the advertiser is still alive
  // even when no controller events fire.  If `advertising` ever flips
  // from true to false without an explicit stop we self-heal by
  // restarting the advertiser.
  private val mainHandler = Handler(Looper.getMainLooper())
  private var advertiseStartSucceeded = false
  private var lastAdvertiseStartMs = 0L
  private var lastAdvertiseFailureCode: Int? = null
  private var watchdogActive = false
  private var advertiseRefreshIndex = 0
  private val advertiserWatchdog = object : Runnable {
    override fun run() {
      logAdvertiserState("watchdog")
      maybeRestartAdvertiserIfStalled()
      // Always rebase the next tick — maybeRestart may have re-queued
      // an immediate run() via startAdvertiserWatchdog, and we don't
      // want the controller log spammed from two stacked tickers.
      if (watchdogActive) {
        mainHandler.removeCallbacks(this)
        mainHandler.postDelayed(this, ADV_WATCHDOG_PERIOD_MS)
      }
    }
  }

  override fun getName(): String = "SpectrePeripheral"

  @ReactMethod
  fun startServer(config: ReadableMap, promise: Promise) {
    try {
      emitLog("startServer requested")
      val useDeviceLocation = resolveUseDeviceLocation(config)
      requireBlePermissions()
      requireLocationForegroundPrereqs(useDeviceLocation)

      val adapter = bluetoothAdapter()
      emitLog(
        "BLE adapter enabled=${adapter.isEnabled} advertiser=${adapter.bluetoothLeAdvertiser != null} multiAdv=${adapter.isMultipleAdvertisementSupported}"
      )

      if (!adapter.isEnabled) {
        emitState(stateMap("Bluetooth is turned off"))
        promise.resolve(stateMap("Bluetooth is turned off"))
        return
      }

      SpectreBleForegroundService.registerStopRequestHandler {
        stopServerFromForegroundService()
      }
      SpectreBleForegroundService.start(reactApplicationContext, useDeviceLocation)
      emitLog(
        "BLE foreground service requested type=${if (useDeviceLocation) "connectedDevice|location" else "connectedDevice"}"
      )
      secureSession.ensurePhonePublicKeyHex()

      gpsBytes = decodeBase64(config.getString("gpsBase64"))
      controlBytes = decodeBase64(config.getString("controlBase64"))
      metadataBytes = (config.getString("metadata") ?: "")
        .toByteArray(StandardCharsets.UTF_8)
      enrichmentBytes = decodeBase64(config.getString("enrichmentBase64"))
      preferredAdvertiseMode = resolveAdvertiseMode(config)
      advertiseMode = preferredAdvertiseMode

      ensureGattServer()
      emitLog("GATT server/services ready")

      emitLog("startAdvertising requested service=$PHONE_SERVICE_UUID mode=${advertiseMode.configValue}")
      startAdvertising(adapter)

      emitState(stateMap())
      promise.resolve(stateMap())
    } catch (error: Exception) {
      runCatching { SpectreBleForegroundService.stop(reactApplicationContext) }
      SpectreBleForegroundService.registerStopRequestHandler(null)
      lastError = error.message ?: "Peripheral start failed"
      emitLog(lastError ?: "Peripheral start failed")
      emitState(stateMap(lastError))
      promise.reject("spectre_peripheral_start", lastError, error)
    }
  }

  @ReactMethod
  fun stopServer(promise: Promise) {
    stopServerInternal(stopForegroundService = true)
    emitState(stateMap())
    promise.resolve(stateMap())
  }

  private fun stopServerFromForegroundService() {
    stopServerInternal(stopForegroundService = false)
    emitLog("Field Mode stopped from foreground notification")
    emitState(stateMap())
  }

  private fun stopServerInternal(stopForegroundService: Boolean) {
    stopAdvertiserWatchdog()
    stopAdvertising()
    connectedDevices.clear()
    ownerDeviceAddress = null
    negotiatedMtuByAddress.clear()
    preparedAuthByAddress.clear()
    preparedEventBatchByAddress.clear()
    preparedStorageByAddress.clear()
    readEnvelopeByPeerAndCharacteristic.clear()
    notificationQueue.clear()
    notificationInFlight = null
    gattServer?.close()
    gattServer = null
    gpsCharacteristic = null
    controlCharacteristic = null
    metadataCharacteristic = null
    eventBatchCharacteristic = null
    enrichmentCharacteristic = null
    authCharacteristic = null
    storageCharacteristic = null
    SpectreBleForegroundService.registerStopRequestHandler(null)
    if (stopForegroundService) {
      SpectreBleForegroundService.stop(reactApplicationContext)
    }
    secureSession.reset()
    advertising = false
    advertiseStartSucceeded = false
    lastAdvertiseFailureCode = null
    advertiseRefreshIndex = 0
    lastError = null
  }

  @ReactMethod
  fun updateMetadata(metadata: String, promise: Promise) {
    metadataBytes = metadata.toByteArray(StandardCharsets.UTF_8)
    metadataCharacteristic?.value = metadataBytes
    clearReadEnvelopeCache(PHONE_METADATA_UUID)
    promise.resolve(null)
  }

  @ReactMethod
  fun updateGpsValue(gpsBase64: String, promise: Promise) {
    gpsBytes = decodeBase64(gpsBase64)
    gpsCharacteristic?.value = gpsBytes
    clearReadEnvelopeCache(PHONE_GPS_UUID)
    notifyCharacteristic(gpsCharacteristic)
    promise.resolve(null)
  }

  @ReactMethod
  fun updateControlValue(controlBase64: String, promise: Promise) {
    controlBytes = decodeBase64(controlBase64)
    controlCharacteristic?.value = controlBytes
    clearReadEnvelopeCache(PHONE_CONTROL_UUID)
    notifyCharacteristic(controlCharacteristic)
    promise.resolve(null)
  }

  @ReactMethod
  fun updateEnrichmentValue(enrichmentBase64: String, notify: Boolean, promise: Promise) {
    enrichmentBytes = decodeBase64(enrichmentBase64)
    enrichmentCharacteristic?.value = enrichmentBytes
    clearReadEnvelopeCache(PHONE_ENRICHMENT_UUID)
    if (notify) {
      notifyCharacteristic(enrichmentCharacteristic)
    }
    promise.resolve(null)
  }

  @ReactMethod
  fun getLastKnownLocation(promise: Promise) {
    try {
      if (!hasPermission(Manifest.permission.ACCESS_FINE_LOCATION)) {
        promise.resolve(null)
        return
      }

      val locationManager =
        reactApplicationContext.getSystemService(Context.LOCATION_SERVICE) as LocationManager

      val providers = listOf(
        LocationManager.GPS_PROVIDER,
        LocationManager.NETWORK_PROVIDER,
        LocationManager.PASSIVE_PROVIDER,
      )

      var best: Location? = null
      for (provider in providers) {
        val candidate = runCatching { locationManager.getLastKnownLocation(provider) }.getOrNull()
        if (candidate != null && (best == null || candidate.time > best.time)) {
          best = candidate
        }
      }

      if (best == null) {
        promise.resolve(null)
        return
      }

      val location = Arguments.createMap().apply {
        putDouble("lat", best.latitude)
        putDouble("lon", best.longitude)
        putDouble("alt", if (best.hasAltitude()) best.altitude else 0.0)
        putDouble("accuracy", if (best.hasAccuracy()) best.accuracy.toDouble() else 0.0)
        putDouble("timestamp", best.time.toDouble())
        putString("provider", best.provider)
      }

      promise.resolve(location)
    } catch (error: Exception) {
      promise.reject("spectre_location", error.message, error)
    }
  }

  @ReactMethod
  fun getAuthPublicKey(promise: Promise) {
    try {
      promise.resolve(secureSession.ensurePhonePublicKeyHex())
    } catch (error: Exception) {
      promise.reject("spectre_auth_public_key", error.message, error)
    }
  }

  @ReactMethod
  fun addListener(eventName: String) {
    // Required for React Native NativeEventEmitter.
  }

  @ReactMethod
  fun removeListeners(count: Int) {
    // Required for React Native NativeEventEmitter.
  }

  private fun ensureGattServer() {
    if (gattServer == null) {
      val bluetoothManager =
        reactApplicationContext.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
      gattServer = bluetoothManager.openGattServer(
        reactApplicationContext,
        serverCallback,
      )
    }

    val server = gattServer ?: throw IllegalStateException("Unable to open GATT server")
    server.clearServices()

    val service = BluetoothGattService(
      PHONE_SERVICE_UUID,
      BluetoothGattService.SERVICE_TYPE_PRIMARY,
    )

    // Keep the Android GATT surface open at the BLE permission layer.
    // Platform credential permissions trigger system prompts; authorization
    // is enforced below by the signed P-256 app session and AES-GCM envelopes.
    gpsCharacteristic = BluetoothGattCharacteristic(
      PHONE_GPS_UUID,
      BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
      BluetoothGattCharacteristic.PERMISSION_READ,
    ).also { characteristic ->
      characteristic.addDescriptor(cccdDescriptor())
      characteristic.value = gpsBytes
      service.addCharacteristic(characteristic)
    }

    controlCharacteristic = BluetoothGattCharacteristic(
      PHONE_CONTROL_UUID,
      BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
      BluetoothGattCharacteristic.PERMISSION_READ,
    ).also { characteristic ->
      characteristic.addDescriptor(cccdDescriptor())
      characteristic.value = controlBytes
      service.addCharacteristic(characteristic)
    }

    metadataCharacteristic = BluetoothGattCharacteristic(
      PHONE_METADATA_UUID,
      BluetoothGattCharacteristic.PROPERTY_READ,
      BluetoothGattCharacteristic.PERMISSION_READ,
    ).also { characteristic ->
      characteristic.value = metadataBytes
      service.addCharacteristic(characteristic)
    }

    eventBatchCharacteristic = BluetoothGattCharacteristic(
      PHONE_EVENT_BATCH_UUID,
      BluetoothGattCharacteristic.PROPERTY_READ or
        BluetoothGattCharacteristic.PROPERTY_WRITE or
        BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
      BluetoothGattCharacteristic.PERMISSION_READ or
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    ).also { characteristic ->
      characteristic.value = eventBatchBytes
      service.addCharacteristic(characteristic)
    }

    enrichmentCharacteristic = BluetoothGattCharacteristic(
      PHONE_ENRICHMENT_UUID,
      BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
      BluetoothGattCharacteristic.PERMISSION_READ,
    ).also { characteristic ->
      characteristic.addDescriptor(cccdDescriptor())
      characteristic.value = enrichmentBytes
      service.addCharacteristic(characteristic)
    }

    storageCharacteristic = BluetoothGattCharacteristic(
      PHONE_STORAGE_UUID,
      BluetoothGattCharacteristic.PROPERTY_READ or
        BluetoothGattCharacteristic.PROPERTY_WRITE or
        BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
      BluetoothGattCharacteristic.PERMISSION_READ or
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    ).also { characteristic ->
      characteristic.value = storageBytes
      service.addCharacteristic(characteristic)
    }

    authCharacteristic = BluetoothGattCharacteristic(
      PHONE_AUTH_UUID,
      BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
      BluetoothGattCharacteristic.PERMISSION_WRITE,
    ).also { characteristic ->
      characteristic.addDescriptor(cccdDescriptor())
      service.addCharacteristic(characteristic)
    }

    if (!server.addService(service)) {
      throw IllegalStateException("Failed to add Spectre companion service")
    }
  }

  private fun startAdvertising(adapter: BluetoothAdapter) {
    if (!adapter.isMultipleAdvertisementSupported) {
      emitLog("WARNING: adapter reports multiple advertisement unsupported")
    }
    advertiser = adapter.bluetoothLeAdvertiser
      ?: throw IllegalStateException("BLE advertiser unavailable")

    stopAdvertising()

    // NOTE: BluetoothLeAdvertiser.startAdvertising(settings, data, callback)
    // always emits LEGACY (non-extended), connectable, scannable PDUs on
    // every Android version we support.  The newer extended advertising
    // path uses BluetoothLeAdvertiser.startAdvertisingSet(...) which we
    // intentionally do NOT call here — NimBLE on the ESP32 only listens
    // for legacy adverts in scan mode.
    val settings = AdvertiseSettings.Builder()
      .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
      .setConnectable(true)
      .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
      .build()

    setAdapterNameForDiagnostics(adapter)

    val serviceUuid = ParcelUuid(PHONE_SERVICE_UUID)
    val dataBuilder = AdvertiseData.Builder()
    val scanResponseBuilder = AdvertiseData.Builder()
    var scanResponse: AdvertiseData? = null

    when (advertiseMode) {
      AdvertisePayloadMode.NAME_ONLY -> {
        // Smallest possible legacy payload that still gives the ESP32
        // something to match: AD flags (auto, 3 bytes) + complete local
        // name "SpectrePhone" (14 bytes) = 17 bytes, well under the
        // 31-byte legacy limit.  No service UUID, no manufacturer data,
        // no tx power — anything else risks Android shortening the
        // local name and our name-fallback match missing the prefix.
        dataBuilder.setIncludeDeviceName(true)
      }

      AdvertisePayloadMode.SHORT_NAME -> {
        // Uses 6-char "SPHONE" adapter name to test whether
        // SpectrePhone's length is what's pushing visibility into
        // marginal territory on the bench radio.
        dataBuilder.setIncludeDeviceName(true)
      }

      AdvertisePayloadMode.UUID_ONLY -> {
        dataBuilder.setIncludeDeviceName(false)
        dataBuilder.addServiceUuid(serviceUuid)
        scanResponse = scanResponseBuilder
          .setIncludeDeviceName(true)
          .build()
      }

      AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE -> {
        // One-run simplification: keep the UUID in the primary payload
        // and omit the scan response name entirely.
        dataBuilder.setIncludeDeviceName(false)
        dataBuilder.addServiceUuid(serviceUuid)
      }
    }

    val data = dataBuilder.build()

    emitLog(
      "advertise payload mode=${advertiseMode.configValue} primaryName=${
        advertiseMode == AdvertisePayloadMode.NAME_ONLY ||
          advertiseMode == AdvertisePayloadMode.SHORT_NAME
      } primaryUuid=${advertiseMode == AdvertisePayloadMode.UUID_ONLY ||
        advertiseMode == AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE} scanName=${scanResponse != null} legacy=true uuid=$PHONE_SERVICE_UUID localName=${adapter.name}"
    )

    advertiseStartSucceeded = false
    lastAdvertiseFailureCode = null
    lastAdvertiseStartMs = System.currentTimeMillis()

    // Byte-budget confirmation: Android may silently truncate or refuse a
    // payload that exceeds the 31-byte legacy AD limit.  Log the key fields
    // so we can correlate a missing advert on the ESP32 side with what the
    // platform actually received here.
    val primaryUuid = advertiseMode == AdvertisePayloadMode.UUID_ONLY ||
        advertiseMode == AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
    emitLog(
      "advertise byte budget mode=${advertiseMode.configValue} primaryUuid=$primaryUuid scanResponse=${scanResponse != null}"
    )

    advertiseCallback = object : AdvertiseCallback() {
      override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
        advertising = true
        advertiseStartSucceeded = true
        lastError = null
        emitLog(
          "Companion peripheral advertising mode=${advertiseMode.configValue} connectable=${settingsInEffect.isConnectable} tx=${settingsInEffect.txPowerLevel} advMode=${settingsInEffect.mode} legacy=true"
        )
        // Confirmed: platform accepted the payload without truncation.
        emitLog(
          "advertise confirmed mode=${advertiseMode.configValue} primaryUuid=$primaryUuid scanResponse=${scanResponse != null}"
        )
        emitState(stateMap())
      }

      override fun onStartFailure(errorCode: Int) {
        advertising = false
        advertiseStartSucceeded = false
        lastAdvertiseFailureCode = errorCode
        lastError = "Advertise failed ($errorCode)"
        emitLog(lastError ?: "Advertise failed")
        emitState(stateMap(lastError))
      }
    }

    if (scanResponse != null) {
      advertiser?.startAdvertising(settings, data, scanResponse, advertiseCallback)
    } else {
      advertiser?.startAdvertising(settings, data, advertiseCallback)
    }

    startAdvertiserWatchdog()
  }

  private fun stopAdvertising() {
    advertiseCallback?.let { callback ->
      advertiser?.stopAdvertising(callback)
    }
    advertiseCallback = null
    advertising = false
    stopAdvertiserWatchdog()
  }

  private fun startAdvertiserWatchdog() {
    if (watchdogActive) {
      return
    }
    watchdogActive = true
    // First tick fires immediately so the bench operator gets a state
    // line right after startServer rather than waiting a full period.
    mainHandler.post(advertiserWatchdog)
  }

  private fun stopAdvertiserWatchdog() {
    watchdogActive = false
    mainHandler.removeCallbacks(advertiserWatchdog)
  }

  private fun logAdvertiserState(reason: String) {
    val adapter = runCatching { bluetoothAdapter() }.getOrNull()
    val ageMs = if (lastAdvertiseStartMs > 0L) {
      System.currentTimeMillis() - lastAdvertiseStartMs
    } else {
      -1L
    }
    val failureNote = lastAdvertiseFailureCode?.let { ",lastFailure=$it" } ?: ""
    emitLog(
      "adv watchdog reason=$reason mode=${advertiseMode.configValue}" +
        " advertising=$advertising callbackOk=$advertiseStartSucceeded" +
        " connected=${connectedDevices.size} localName=${adapter?.name ?: "?"}" +
        " ageMs=$ageMs$failureNote"
    )
  }

  private fun maybeRestartAdvertiserIfStalled() {
    // If startAdvertising's onStartSuccess never fired but startServer
    // claims we're advertising, or if `advertising` flipped to false
    // without an explicit stop call (e.g. the GATT server still alive
    // but the controller dropped us), kick the advertiser back up so
    // ESP32 manual BTCON has a chance to find us in the next scan.
    if (gattServer == null) {
      return
    }
    val adapter = runCatching { bluetoothAdapter() }.getOrNull() ?: return
    if (!adapter.isEnabled) {
      return
    }
    val ageMs = if (lastAdvertiseStartMs > 0L) {
      System.currentTimeMillis() - lastAdvertiseStartMs
    } else {
      Long.MAX_VALUE
    }

    if (advertising && advertiseStartSucceeded) {
      val refreshPeriodMs = if (connectedDevices.isEmpty()) {
        BEACON_REFRESH_PERIOD_MS
      } else {
        CONNECTED_BEACON_REFRESH_PERIOD_MS
      }
      if (ageMs >= refreshPeriodMs) {
        val nextMode = nextIdleBeaconMode()
        emitLog(
          "adv beacon refresh mode=${advertiseMode.configValue}->${nextMode.configValue} connected=${connectedDevices.size} ageMs=$ageMs"
        )
        advertiseMode = nextMode
        totalAdvertiseRestarts += 1
        runCatching { startAdvertising(adapter) }.onFailure { error ->
          emitLog("adv beacon refresh failed: ${error.message}")
        }
      }
      return
    }

    // Give startAdvertising() time to deliver onStartSuccess before we
    // decide the advertiser has stalled.  Without this, the first watchdog
    // tick (posted immediately after startAdvertising) fires before the
    // callback arrives and triggers a spurious restart cycle.
    if (ageMs < ADVERTISE_START_GRACE_MS && !advertiseStartSucceeded) {
      Log.i(LOG_TAG, "adv watchdog grace ageMs=$ageMs")
      return
    }

    emitLog("adv watchdog restart: advertising=$advertising callbackOk=$advertiseStartSucceeded")
    totalAdvertiseRestarts += 1
    runCatching { startAdvertising(adapter) }.onFailure { error ->
      emitLog("adv watchdog restart failed: ${error.message}")
    }
  }

  private fun nextIdleBeaconMode(): AdvertisePayloadMode {
    advertiseRefreshIndex += 1
    return when (preferredAdvertiseMode) {
      AdvertisePayloadMode.NAME_ONLY,
      AdvertisePayloadMode.SHORT_NAME -> preferredAdvertiseMode
      AdvertisePayloadMode.UUID_ONLY -> AdvertisePayloadMode.UUID_ONLY
      AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE ->
        AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
    }
  }

  private fun notifyCharacteristic(characteristic: BluetoothGattCharacteristic?) {
    if (characteristic == null) {
      return
    }

    val payload = characteristic.value ?: ByteArray(0)
    for (device in connectedDevices.values) {
      if (!isAuthorized(device)) {
        emitLog("notify skipped label=${characteristicLabel(characteristic)} peer=${addressLabel(device)} reason=unauthenticated")
        continue
      }
      enqueueNotification(device, characteristic, payload)
    }
  }

  private fun enqueueNotification(
    device: BluetoothDevice,
    characteristic: BluetoothGattCharacteristic,
    payload: ByteArray,
  ) {
    val label = characteristicLabel(characteristic)
    val channel = secureChannelForCharacteristic(characteristic.uuid)
    val attPayloadBytes = if (characteristic.uuid == PHONE_AUTH_UUID) {
      maxOf(notificationPayloadBytes(device), payload.size.coerceAtMost(MAX_NOTIFICATION_PAYLOAD_BYTES))
    } else {
      notificationPayloadBytes(device)
    }
    val chunkSize = if (channel != null) {
      secureSession.maxPlaintextForPayload(attPayloadBytes)
    } else {
      attPayloadBytes
    }
    if (chunkSize <= 0) {
      emitLog("notify skipped label=$label peer=${addressLabel(device)} reason=mtu_too_small attPayload=$attPayloadBytes")
      return
    }

    val total = maxOf(1, (payload.size + chunkSize - 1) / chunkSize)

    if (total > 1 || characteristic.uuid == PHONE_ENRICHMENT_UUID) {
      emitLog(
        "notify queued label=$label peer=${addressLabel(device)} plain=${payload.size} chunk=$chunkSize chunks=$total secure=${channel != null}"
      )
    }

    fun enqueueChunk(chunk: ByteArray, index: Int) {
      val sealed = try {
        if (channel == null) chunk else secureSession.encrypt(channel, chunk)
      } catch (error: Exception) {
        emitLog("notify encrypt failed label=$label peer=${addressLabel(device)} error=${error.message}")
        return
      }
      notificationQueue.add(
        PendingNotification(device, characteristic, sealed, label, index, total)
      )
    }

    if (payload.isEmpty()) {
      enqueueChunk(ByteArray(0), 1)
    } else {
      var offset = 0
      var index = 1
      while (offset < payload.size) {
        val end = minOf(offset + chunkSize, payload.size)
        enqueueChunk(payload.copyOfRange(offset, end), index)
        offset = end
        index += 1
      }
    }

    drainNotificationQueue()
  }

  private fun drainNotificationQueue() {
    if (notificationInFlight != null) {
      return
    }

    val server = gattServer ?: run {
      notificationQueue.clear()
      return
    }

    val next = notificationQueue.poll() ?: return
    notificationInFlight = next
    next.characteristic.value = next.value

    val accepted = try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        server.notifyCharacteristicChanged(next.device, next.characteristic, false, next.value) == 0
      } else {
        @Suppress("DEPRECATION")
        server.notifyCharacteristicChanged(next.device, next.characteristic, false)
      }
    } catch (error: SecurityException) {
      lastError = error.message
      emitLog(lastError ?: "Characteristic notify failed")
      false
    }

    if (!accepted) {
      emitLog(
        "notify not accepted label=${next.label} peer=${addressLabel(next.device)} chunk=${next.index}/${next.total} bytes=${next.value.size}"
      )
      notificationInFlight = null
      drainNotificationQueue()
      return
    }

    if (next.total > 1) {
      emitLog(
        "notify sent label=${next.label} peer=${addressLabel(next.device)} chunk=${next.index}/${next.total} bytes=${next.value.size}"
      )
    }

  }

  private fun notificationPayloadBytes(device: BluetoothDevice): Int {
    val mtu = negotiatedMtuByAddress[device.address]
    if (mtu == null || mtu <= 3) {
      return DEFAULT_NOTIFICATION_PAYLOAD_BYTES
    }
    return (mtu - 3)
      .coerceAtLeast(DEFAULT_NOTIFICATION_PAYLOAD_BYTES)
      .coerceAtMost(MAX_NOTIFICATION_PAYLOAD_BYTES)
  }

  private fun characteristicLabel(characteristic: BluetoothGattCharacteristic): String =
    when (characteristic.uuid) {
      PHONE_GPS_UUID -> "gps"
      PHONE_CONTROL_UUID -> "control"
      PHONE_METADATA_UUID -> "metadata"
      PHONE_EVENT_BATCH_UUID -> "eventBatch"
      PHONE_ENRICHMENT_UUID -> "enrichment"
      PHONE_AUTH_UUID -> "auth"
      PHONE_STORAGE_UUID -> "storage"
      else -> characteristic.uuid.toString()
    }

  private fun secureChannelForCharacteristic(uuid: UUID): Int? =
    when (uuid) {
      PHONE_GPS_UUID -> SpectreSecureSession.CHANNEL_GPS
      PHONE_CONTROL_UUID -> SpectreSecureSession.CHANNEL_CONTROL
      PHONE_METADATA_UUID -> SpectreSecureSession.CHANNEL_META
      PHONE_EVENT_BATCH_UUID -> SpectreSecureSession.CHANNEL_EVENT_BATCH
      PHONE_ENRICHMENT_UUID -> SpectreSecureSession.CHANNEL_ENRICHMENT
      PHONE_STORAGE_UUID -> SpectreSecureSession.CHANNEL_STORAGE
      else -> null
    }

  private fun stateMap(errorOverride: String? = null): WritableMap =
    Arguments.createMap().apply {
      putBoolean("running", gattServer != null)
      putBoolean("advertising", advertising)
      putInt("connectedDevices", connectedDevices.size)
      putBoolean("secureSessionReady", secureSession.isReady)
      putString("advertiseMode", advertiseMode.configValue)
      putBoolean("advertiseStartConfirmed", advertiseStartSucceeded)
      putBoolean("watchdogActive", watchdogActive)
      putInt("totalAdvertiseRestarts", totalAdvertiseRestarts)
      putNullableTimestamp("lastAdvertiseStartedAt", lastAdvertiseStartMs)
      if (lastAdvertiseFailureCode == null) {
        putNull("lastAdvertiseFailureCode")
      } else {
        putInt("lastAdvertiseFailureCode", lastAdvertiseFailureCode ?: 0)
      }
      putNullableTimestamp("lastConnectedAt", lastConnectedAtMs)
      putNullableTimestamp("lastDisconnectedAt", lastDisconnectedAtMs)
      putString("lastConnectedPeer", lastConnectedPeerLabel)
      putString("lastDisconnectedPeer", lastDisconnectedPeerLabel)
      putNullableTimestamp("lastBatchReceivedAt", lastBatchReceivedAtMs)
      putString("lastBatchPeer", lastBatchPeerLabel)
      putInt("lastBatchBytes", lastBatchBytes)
      putInt("lastBatchRecords", lastBatchRecords)
      putNullableTimestamp("lastStorageReceivedAt", lastStorageReceivedAtMs)
      putString("lastStoragePeer", lastStoragePeerLabel)
      putString("storageBase64", if (storageBytes.isEmpty()) null else encodeBase64(storageBytes))
      putInt("totalBatchesReceived", totalBatchesReceived)
      putInt("totalBatchBytes", totalBatchBytes)
      putInt("totalBatchRecords", totalBatchRecords)
      putString("error", errorOverride ?: lastError)
    }

  private fun WritableMap.putNullableTimestamp(key: String, value: Long) {
    if (value > 0L) {
      putDouble(key, value.toDouble())
    } else {
      putNull(key)
    }
  }

  private fun emitState(state: WritableMap) {
    emitEvent("SpectrePeripheralState", state)
  }

  private fun emitLog(message: String) {
    Log.i(LOG_TAG, message)

    emitEvent(
      "SpectrePeripheralLog",
      Arguments.createMap().apply {
        putString("message", message)
      },
    )
  }

  private fun emitEventBatch(base64Payload: String, payloadSize: Int) {
    emitEvent(
      "SpectrePeripheralEventBatch",
      Arguments.createMap().apply {
        putString("base64", base64Payload)
        putInt("length", payloadSize)
        putDouble("receivedAt", System.currentTimeMillis().toDouble())
      },
    )
  }

  private fun emitStorageSnapshot(base64Payload: String, payloadSize: Int) {
    emitEvent(
      "SpectrePeripheralStorage",
      Arguments.createMap().apply {
        putString("base64", base64Payload)
        putInt("length", payloadSize)
        putDouble("receivedAt", System.currentTimeMillis().toDouble())
      },
    )
  }

  private fun emitEvent(name: String, payload: WritableMap) {
    if (!reactApplicationContext.hasActiveCatalystInstance()) {
      return
    }

    reactApplicationContext
      .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
      .emit(name, payload)
  }

  private fun decodeBase64(value: String?): ByteArray {
    if (value.isNullOrBlank()) {
      return ByteArray(0)
    }
    return Base64.decode(value, Base64.NO_WRAP)
  }

  private fun encodeBase64(value: ByteArray): String =
    Base64.encodeToString(value, Base64.NO_WRAP)

  private fun requireBlePermissions() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
      return
    }

    if (!hasPermission(Manifest.permission.BLUETOOTH_CONNECT) ||
      !hasPermission(Manifest.permission.BLUETOOTH_ADVERTISE)
    ) {
      throw SecurityException("Bluetooth connect/advertise permission missing")
    }
  }

  private fun requireLocationForegroundPrereqs(useDeviceLocation: Boolean) {
    if (!useDeviceLocation) {
      return
    }

    if (!hasPermission(Manifest.permission.ACCESS_FINE_LOCATION) &&
      !hasPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
    ) {
      throw SecurityException("Location permission missing for phone GPS Field Mode")
    }

    val locationManager =
      reactApplicationContext.getSystemService(Context.LOCATION_SERVICE) as LocationManager
    val locationEnabled = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      locationManager.isLocationEnabled
    } else {
      @Suppress("DEPRECATION")
      locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
        locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
    }

    if (!locationEnabled) {
      throw SecurityException("Android location services are disabled")
    }
  }

  private fun hasPermission(permission: String): Boolean =
    ContextCompat.checkSelfPermission(
      reactApplicationContext,
      permission,
    ) == PackageManager.PERMISSION_GRANTED

  private fun bluetoothAdapter(): BluetoothAdapter {
    val bluetoothManager =
      reactApplicationContext.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    return bluetoothManager.adapter
      ?: throw IllegalStateException("Bluetooth adapter unavailable")
  }

  private fun cccdDescriptor(): BluetoothGattDescriptor =
    BluetoothGattDescriptor(
      CCCD_UUID,
      BluetoothGattDescriptor.PERMISSION_READ or
        BluetoothGattDescriptor.PERMISSION_WRITE,
    )

  private fun resolveAdvertiseMode(config: ReadableMap): AdvertisePayloadMode {
    val requested = if (config.hasKey("advertiseMode")) {
      config.getString("advertiseMode")
    } else {
      null
    }

    val normalized = requested
      ?.trim()
      ?.replace("-", "")
      ?.replace("_", "")
      ?.lowercase()

    return when (normalized) {
      "nameonly" -> AdvertisePayloadMode.NAME_ONLY
      "uuidonly" -> AdvertisePayloadMode.UUID_ONLY
      "shortname", "short" -> AdvertisePayloadMode.SHORT_NAME
      "service", "uuid" -> AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
      null, "" -> {
        if (BuildConfig.DEBUG) {
          AdvertisePayloadMode.NAME_ONLY
        } else {
          AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
        }
      }
      else -> {
        emitLog("Unknown advertiseMode '$requested'; using service UUID mode")
        AdvertisePayloadMode.SERVICE_UUID_WITH_NAME_SCAN_RESPONSE
      }
    }
  }

  /** Adapter local-name we want the controller to broadcast for the current mode. */
  private fun desiredLocalName(mode: AdvertisePayloadMode): String =
    if (mode == AdvertisePayloadMode.SHORT_NAME) DEVICE_NAME_SHORT else DEVICE_NAME

  private fun resolveUseDeviceLocation(config: ReadableMap): Boolean =
    config.hasKey("useDeviceLocation") &&
      !config.isNull("useDeviceLocation") &&
      config.getBoolean("useDeviceLocation")

  private fun setAdapterNameForDiagnostics(adapter: BluetoothAdapter) {
    val desired = desiredLocalName(advertiseMode)
    try {
      if (adapter.name != desired) {
        @Suppress("DEPRECATION")
        adapter.name = desired
      }
      emitLog("BLE local name requested=$desired actual=${adapter.name} mode=${advertiseMode.configValue}")
    } catch (error: SecurityException) {
      emitLog("BLE local name not changed: ${error.message}")
    }
  }

  private fun mergeBytes(existing: ByteArray, offset: Int, value: ByteArray): ByteArray {
    if (offset <= 0) {
      return value.copyOf()
    }

    val nextSize = maxOf(existing.size, offset + value.size)
    val merged = ByteArray(nextSize)
    if (existing.isNotEmpty()) {
      System.arraycopy(existing, 0, merged, 0, existing.size)
    }
    System.arraycopy(value, 0, merged, offset, value.size)
    return merged
  }

  private fun mergeEventBatch(offset: Int, value: ByteArray) {
    eventBatchBytes = mergeBytes(eventBatchBytes, offset, value)
    eventBatchCharacteristic?.value = eventBatchBytes
    clearReadEnvelopeCache(PHONE_EVENT_BATCH_UUID)
  }

  private fun mergeStorage(offset: Int, value: ByteArray) {
    storageBytes = mergeBytes(storageBytes, offset, value)
    storageCharacteristic?.value = storageBytes
    clearReadEnvelopeCache(PHONE_STORAGE_UUID)
  }

  private fun commitEventBatch(device: BluetoothDevice, bytes: ByteArray) {
    if (
      bytes.isEmpty() ||
      bytes.size > PHONE_EVENT_BATCH_MAX_BYTES ||
      bytes.size % EVENT_BATCH_RECORD_SIZE != 0
    ) {
      emitLog("eventBatch dropped peer=${addressLabel(device)} bytes=${bytes.size}")
      return
    }

    eventBatchBytes = bytes
    eventBatchCharacteristic?.value = eventBatchBytes
    clearReadEnvelopeCache(PHONE_EVENT_BATCH_UUID)
    lastBatchBytes = bytes.size
    lastBatchRecords = bytes.size / EVENT_BATCH_RECORD_SIZE
    lastBatchReceivedAtMs = System.currentTimeMillis()
    lastBatchPeerLabel = addressLabel(device)
    totalBatchesReceived += 1
    totalBatchBytes += lastBatchBytes
    totalBatchRecords += lastBatchRecords
    emitLog(
      "eventBatch accepted peer=${addressLabel(device)} records=$lastBatchRecords bytes=$lastBatchBytes totalBatches=$totalBatchesReceived"
    )
    emitState(stateMap())
    emitEventBatch(encodeBase64(eventBatchBytes), eventBatchBytes.size)
  }

  private fun commitStorageFrame(device: BluetoothDevice, bytes: ByteArray) {
    if (bytes.size != PHONE_STORAGE_FRAME_SIZE) {
      emitLog("storage snapshot dropped peer=${addressLabel(device)} bytes=${bytes.size}")
      return
    }

    storageBytes = bytes
    storageCharacteristic?.value = storageBytes
    clearReadEnvelopeCache(PHONE_STORAGE_UUID)
    lastStorageReceivedAtMs = System.currentTimeMillis()
    lastStoragePeerLabel = addressLabel(device)
    emitLog("storage snapshot accepted peer=${addressLabel(device)} bytes=${bytes.size}")
    emitState(stateMap())
    emitStorageSnapshot(encodeBase64(storageBytes), storageBytes.size)
  }

  private fun clearReadEnvelopeCache(uuid: UUID? = null) {
    if (uuid == null) {
      readEnvelopeByPeerAndCharacteristic.clear()
      return
    }
    val suffix = "|$uuid"
    readEnvelopeByPeerAndCharacteristic.keys
      .filter { it.endsWith(suffix) }
      .forEach { readEnvelopeByPeerAndCharacteristic.remove(it) }
  }

  private fun readEnvelopeKey(device: BluetoothDevice, uuid: UUID): String =
    "${device.address}|$uuid"

  private fun addressLabel(device: BluetoothDevice): String {
    val address = device.address ?: return "unknown"
    return if (address.length <= 5) address else "...${address.takeLast(5)}"
  }

  private fun ownsSession(device: BluetoothDevice): Boolean =
    ownerDeviceAddress == device.address

  private fun isAuthorized(device: BluetoothDevice): Boolean =
    ownsSession(device) && secureSession.isReady

  private fun sendAuthResponse(device: BluetoothDevice, response: ByteArray) {
    val characteristic = authCharacteristic ?: run {
      emitLog("auth response dropped peer=${addressLabel(device)} reason=missing_char")
      return
    }
    enqueueNotification(device, characteristic, response)
  }

  private fun rejectUnauthorized(
    device: BluetoothDevice,
    requestId: Int,
    offset: Int,
    operation: String,
  ) {
    emitLog("GATT $operation rejected unauthenticated peer=${addressLabel(device)}")
    gattServer?.sendResponse(
      device,
      requestId,
      GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
      offset,
      null,
    )
  }

  private val serverCallback = object : BluetoothGattServerCallback() {
    override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
      emitLog("GATT state peer=${addressLabel(device)} status=$status newState=$newState")
      if (newState == BluetoothProfile.STATE_CONNECTED) {
        val currentOwner = ownerDeviceAddress
        if (currentOwner == null) {
          ownerDeviceAddress = device.address
          secureSession.reset()
          clearReadEnvelopeCache()
          emitLog("Spectre companion owner set peer=${addressLabel(device)}")
        } else if (currentOwner != device.address) {
          emitLog("Spectre companion rejected extra peer=${addressLabel(device)}")
          gattServer?.cancelConnection(device)
          emitState(stateMap())
          return
        }
        connectedDevices[device.address] = device
        lastConnectedAtMs = System.currentTimeMillis()
        lastConnectedPeerLabel = addressLabel(device)
        emitLog("Spectre companion connected ${addressLabel(device)}")
      } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
        connectedDevices.remove(device.address)
        lastDisconnectedAtMs = System.currentTimeMillis()
        lastDisconnectedPeerLabel = addressLabel(device)
        negotiatedMtuByAddress.remove(device.address)
        preparedAuthByAddress.remove(device.address)
        preparedEventBatchByAddress.remove(device.address)
        preparedStorageByAddress.remove(device.address)
        readEnvelopeByPeerAndCharacteristic.keys
          .filter { it.startsWith("${device.address}|") }
          .forEach { readEnvelopeByPeerAndCharacteristic.remove(it) }
        notificationQueue.removeAll { it.device.address == device.address }
        if (notificationInFlight?.device?.address == device.address) {
          notificationInFlight = null
          drainNotificationQueue()
        }
        if (ownerDeviceAddress == device.address) {
          ownerDeviceAddress = null
          secureSession.reset()
          clearReadEnvelopeCache()
          emitLog("Spectre companion secure session cleared peer=${addressLabel(device)}")
        }
        emitLog("Spectre companion disconnected ${addressLabel(device)}")
      }
      emitState(stateMap())
    }

    override fun onMtuChanged(device: BluetoothDevice, mtu: Int) {
      if (!ownsSession(device)) {
        emitLog("GATT MTU ignored non-owner peer=${addressLabel(device)}")
        return
      }
      negotiatedMtuByAddress[device.address] = mtu
      emitLog(
        "GATT MTU peer=${addressLabel(device)} mtu=$mtu notifyPayload=${notificationPayloadBytes(device)}"
      )
    }

    override fun onNotificationSent(device: BluetoothDevice, status: Int) {
      val sent = notificationInFlight
      if (sent == null || sent.device.address != device.address) {
        emitLog("notify sent callback without in-flight peer=${addressLabel(device)} status=$status")
        return
      }

      if (status != BluetoothGatt.GATT_SUCCESS) {
        emitLog(
          "notify callback status=$status label=${sent.label} peer=${addressLabel(device)} chunk=${sent.index}/${sent.total}"
        )
      }

      notificationInFlight = null
      drainNotificationQueue()
    }

    override fun onCharacteristicReadRequest(
      device: BluetoothDevice,
      requestId: Int,
      offset: Int,
      characteristic: BluetoothGattCharacteristic,
    ) {
      if (characteristic.uuid == PHONE_AUTH_UUID) {
        gattServer?.sendResponse(
          device,
          requestId,
          BluetoothGatt.GATT_SUCCESS,
          offset,
          ByteArray(0),
        )
        return
      }

      if (!isAuthorized(device)) {
        rejectUnauthorized(device, requestId, offset, "read")
        return
      }

      val payload = try {
        val channel = secureChannelForCharacteristic(characteristic.uuid)
        val plaintext = when (characteristic.uuid) {
          PHONE_GPS_UUID -> gpsBytes
          PHONE_CONTROL_UUID -> controlBytes
          PHONE_METADATA_UUID -> metadataBytes
          PHONE_EVENT_BATCH_UUID -> eventBatchBytes
          PHONE_ENRICHMENT_UUID -> enrichmentBytes
          PHONE_STORAGE_UUID -> storageBytes
          else -> ByteArray(0)
        }
        if (channel == null) {
          plaintext
        } else if (offset == 0) {
          secureSession.encrypt(channel, plaintext).also { sealed ->
            readEnvelopeByPeerAndCharacteristic[readEnvelopeKey(device, characteristic.uuid)] = sealed
          }
        } else {
          readEnvelopeByPeerAndCharacteristic[readEnvelopeKey(device, characteristic.uuid)]
            ?: secureSession.encrypt(channel, plaintext).also { sealed ->
              readEnvelopeByPeerAndCharacteristic[readEnvelopeKey(device, characteristic.uuid)] = sealed
            }
        }
      } catch (error: Exception) {
        emitLog("GATT read encrypt failed peer=${addressLabel(device)} label=${characteristicLabel(characteristic)} error=${error.message}")
        gattServer?.sendResponse(
          device,
          requestId,
          GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
          offset,
          null,
        )
        return
      }

      val safeOffset = offset.coerceAtMost(payload.size)
      val response =
        if (safeOffset >= payload.size) ByteArray(0) else payload.copyOfRange(safeOffset, payload.size)

      gattServer?.sendResponse(
        device,
        requestId,
        BluetoothGatt.GATT_SUCCESS,
        safeOffset,
        response,
      )
    }

    override fun onCharacteristicWriteRequest(
      device: BluetoothDevice,
      requestId: Int,
      characteristic: BluetoothGattCharacteristic,
      preparedWrite: Boolean,
      responseNeeded: Boolean,
      offset: Int,
      value: ByteArray,
    ) {
      if (characteristic.uuid == PHONE_AUTH_UUID) {
        if (!ownsSession(device)) {
          rejectUnauthorized(device, requestId, offset, "authWrite")
          return
        }
        if (preparedWrite) {
          val merged = mergeBytes(
            preparedAuthByAddress[device.address] ?: ByteArray(0),
            offset,
            value,
          )
          preparedAuthByAddress[device.address] = merged
          emitLog(
            "auth prepared peer=${addressLabel(device)} offset=$offset chunk=${value.size} total=${merged.size}"
          )
          if (responseNeeded) {
            gattServer?.sendResponse(
              device,
              requestId,
              BluetoothGatt.GATT_SUCCESS,
              offset,
              value,
            )
          }
          return
        }
        try {
          val response = secureSession.handleChallenge(value)
          if (responseNeeded) {
            gattServer?.sendResponse(
              device,
              requestId,
              BluetoothGatt.GATT_SUCCESS,
              offset,
              null,
            )
          }
          sendAuthResponse(device, response)
          emitState(stateMap())
        } catch (error: Exception) {
          secureSession.reset()
          emitLog("auth challenge rejected peer=${addressLabel(device)} error=${error.message}")
          if (responseNeeded) {
            gattServer?.sendResponse(
              device,
              requestId,
              GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
              offset,
              null,
            )
          }
          gattServer?.cancelConnection(device)
          emitState(stateMap(error.message))
        }
        return
      }

      if (!isAuthorized(device)) {
        rejectUnauthorized(device, requestId, offset, "write")
        return
      }

      if (characteristic.uuid == PHONE_EVENT_BATCH_UUID) {
        if (preparedWrite) {
          val merged = mergeBytes(
            preparedEventBatchByAddress[device.address] ?: ByteArray(0),
            offset,
            value,
          )
          preparedEventBatchByAddress[device.address] = merged
          emitLog(
            "eventBatch prepared peer=${addressLabel(device)} offset=$offset chunk=${value.size} total=${merged.size}"
          )
        } else {
          try {
            val plaintext = secureSession.decrypt(
              SpectreSecureSession.CHANNEL_EVENT_BATCH,
              value,
            )
            mergeEventBatch(offset, plaintext)
            commitEventBatch(device, eventBatchBytes)
          } catch (error: Exception) {
            emitLog("eventBatch decrypt failed peer=${addressLabel(device)} bytes=${value.size} error=${error.message}")
            if (responseNeeded) {
              gattServer?.sendResponse(
                device,
                requestId,
                GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
                offset,
                null,
              )
            }
            gattServer?.cancelConnection(device)
            return
          }
        }
      } else if (characteristic.uuid == PHONE_STORAGE_UUID) {
        if (preparedWrite) {
          val merged = mergeBytes(
            preparedStorageByAddress[device.address] ?: ByteArray(0),
            offset,
            value,
          )
          preparedStorageByAddress[device.address] = merged
          emitLog(
            "storage prepared peer=${addressLabel(device)} offset=$offset chunk=${value.size} total=${merged.size}"
          )
        } else {
          try {
            val plaintext = secureSession.decrypt(
              SpectreSecureSession.CHANNEL_STORAGE,
              value,
            )
            mergeStorage(offset, plaintext)
            commitStorageFrame(device, storageBytes)
          } catch (error: Exception) {
            emitLog("storage decrypt failed peer=${addressLabel(device)} bytes=${value.size} error=${error.message}")
            if (responseNeeded) {
              gattServer?.sendResponse(
                device,
                requestId,
                GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
                offset,
                null,
              )
            }
            gattServer?.cancelConnection(device)
            return
          }
        }
      }

      if (responseNeeded) {
        gattServer?.sendResponse(
          device,
          requestId,
          BluetoothGatt.GATT_SUCCESS,
          offset,
          if (preparedWrite) value else null,
        )
      }
    }

    override fun onExecuteWrite(device: BluetoothDevice, requestId: Int, execute: Boolean) {
      val stagedAuth = preparedAuthByAddress.remove(device.address)
      if (stagedAuth != null) {
        if (!ownsSession(device)) {
          rejectUnauthorized(device, requestId, 0, "authExecuteWrite")
          return
        }
        if (!execute) {
          emitLog("auth execute cancelled peer=${addressLabel(device)} bytes=${stagedAuth.size}")
          gattServer?.sendResponse(
            device,
            requestId,
            BluetoothGatt.GATT_SUCCESS,
            0,
            null,
          )
          return
        }
        try {
          val response = secureSession.handleChallenge(stagedAuth)
          gattServer?.sendResponse(
            device,
            requestId,
            BluetoothGatt.GATT_SUCCESS,
            0,
            null,
          )
          sendAuthResponse(device, response)
          emitState(stateMap())
        } catch (error: Exception) {
          secureSession.reset()
          emitLog("auth execute rejected peer=${addressLabel(device)} bytes=${stagedAuth.size} error=${error.message}")
          gattServer?.sendResponse(
            device,
            requestId,
            GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
            0,
            null,
          )
          gattServer?.cancelConnection(device)
          emitState(stateMap(error.message))
        }
        return
      }

      if (!isAuthorized(device)) {
        rejectUnauthorized(device, requestId, 0, "executeWrite")
        return
      }

      val stagedStorage = preparedStorageByAddress.remove(device.address)
      if (execute && stagedStorage != null) {
        try {
          val plaintext = secureSession.decrypt(
            SpectreSecureSession.CHANNEL_STORAGE,
            stagedStorage,
          )
          emitLog("storage execute peer=${addressLabel(device)} sealed=${stagedStorage.size} plain=${plaintext.size}")
          commitStorageFrame(device, plaintext)
        } catch (error: Exception) {
          emitLog("storage execute decrypt failed peer=${addressLabel(device)} bytes=${stagedStorage.size} error=${error.message}")
          gattServer?.sendResponse(
            device,
            requestId,
            GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
            0,
            null,
          )
          gattServer?.cancelConnection(device)
          return
        }

        gattServer?.sendResponse(
          device,
          requestId,
          BluetoothGatt.GATT_SUCCESS,
          0,
          null,
        )
        return
      } else if (stagedStorage != null) {
        emitLog("storage execute cancelled peer=${addressLabel(device)} bytes=${stagedStorage.size}")
      }

      val staged = preparedEventBatchByAddress.remove(device.address)
      if (execute && staged != null) {
        try {
          val plaintext = secureSession.decrypt(
            SpectreSecureSession.CHANNEL_EVENT_BATCH,
            staged,
          )
          emitLog("eventBatch execute peer=${addressLabel(device)} sealed=${staged.size} plain=${plaintext.size}")
          commitEventBatch(device, plaintext)
        } catch (error: Exception) {
          emitLog("eventBatch execute decrypt failed peer=${addressLabel(device)} bytes=${staged.size} error=${error.message}")
          gattServer?.sendResponse(
            device,
            requestId,
            GATT_INSUFFICIENT_AUTHORIZATION_STATUS,
            0,
            null,
          )
          gattServer?.cancelConnection(device)
          return
        }
      } else if (staged != null) {
        emitLog("eventBatch execute cancelled peer=${addressLabel(device)} bytes=${staged.size}")
      }

      gattServer?.sendResponse(
        device,
        requestId,
        BluetoothGatt.GATT_SUCCESS,
        0,
        null,
      )
    }
  }
}
