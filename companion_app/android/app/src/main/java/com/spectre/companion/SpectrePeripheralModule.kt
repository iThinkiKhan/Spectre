@file:Suppress("DEPRECATION")

package com.spectre.companion

import android.Manifest
import android.annotation.SuppressLint
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
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.content.Context
import android.content.Intent
import android.location.Location
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Base64
import android.util.Log
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
import java.lang.ref.WeakReference
import java.nio.charset.StandardCharsets
import java.util.LinkedHashMap
import java.util.UUID

class SpectrePeripheralModule(
    private val reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext), LifecycleEventListener {
  private data class StartConfig(
      val metadata: String,
      val gpsBase64: String,
      val controlBase64: String,
      val enrichmentBase64: String,
      val advertiseMode: String,
      val useDeviceLocation: Boolean,
  )

  private data class PeripheralState(
      var running: Boolean = false,
      var advertising: Boolean = false,
      var connectedDevices: Int = 0,
      var secureSessionReady: Boolean = false,
      var advertiseMode: String? = null,
      var advertiseStartConfirmed: Boolean = false,
      var watchdogActive: Boolean = false,
      var totalAdvertiseRestarts: Int = 0,
      var lastAdvertiseStartedAt: Long? = null,
      var lastAdvertiseFailureCode: Int? = null,
      var lastConnectedAt: Long? = null,
      var lastDisconnectedAt: Long? = null,
      var lastConnectedPeer: String? = null,
      var lastDisconnectedPeer: String? = null,
      var lastBatchReceivedAt: Long? = null,
      var lastBatchPeer: String? = null,
      var lastBatchBytes: Int = 0,
      var lastBatchRecords: Int = 0,
      var lastStorageReceivedAt: Long? = null,
      var lastStoragePeer: String? = null,
      var storageBase64: String? = null,
      var totalBatchesReceived: Int = 0,
      var totalBatchBytes: Long = 0,
      var totalBatchRecords: Long = 0,
      var error: String? = null,
      var moduleAvailable: Boolean = true,
  )

  private val handler = Handler(Looper.getMainLooper())
  private val bluetoothManager = reactContext.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
  private val bluetoothAdapter: BluetoothAdapter? get() = bluetoothManager.adapter
  private val connectedDevices = LinkedHashMap<String, BluetoothDevice>()
  private val characteristicCache = mutableMapOf<UUID, ByteArray>()
  private val commandChannel = SpectrePeripheralCommandChannel()
  private val secureSession = BleSecureSession()
  private val state = PeripheralState()

  private var bluetoothGattServer: BluetoothGattServer? = null
  private var bluetoothAdvertiser: BluetoothLeAdvertiser? = null
  private var advertiseCallback: AdvertiseCallback? = null
  private var service: BluetoothGattService? = null
  private var useDeviceLocation = false
  private val advertiseWatchdog =
      object : Runnable {
        override fun run() {
          if (!state.running) {
            return
          }

          val now = System.currentTimeMillis()
          val lastStart = state.lastAdvertiseStartedAt ?: 0L

          when {
            advertiseCallback == null || !state.advertising -> restartAdvertising("watchdog")
            lastStart == 0L || now - lastStart >= ADVERTISE_KICK_INTERVAL_MS ->
                restartAdvertising("periodic_kick")
          }

          handler.postDelayed(this, ADVERTISE_WATCHDOG_INTERVAL_MS)
        }
      }

  init {
    reactContext.addLifecycleEventListener(this)
    activeModule = WeakReference(this)
  }

  override fun getName(): String = "SpectrePeripheral"

  override fun onHostResume() = Unit

  override fun onHostPause() = Unit

  override fun onHostDestroy() {
    stopInternal()
    if (activeModule?.get() === this) {
      activeModule = null
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun addListener(eventName: String?) = Unit

  @com.facebook.react.bridge.ReactMethod
  fun removeListeners(count: Double) = Unit

  @com.facebook.react.bridge.ReactMethod
  fun startServer(config: ReadableMap, promise: Promise) {
    runOnMain {
      try {
        if (state.running) {
          stopInternal()
        }

        val parsed = parseStartConfig(config)
        traceInfo(
            "start_server",
            "mode" to parsed.advertiseMode,
            "useDeviceLocation" to parsed.useDeviceLocation,
            "metadataBytes" to parsed.metadata.toByteArray(StandardCharsets.UTF_8).size,
            "gpsBytes" to decodeBase64OrEmpty(parsed.gpsBase64).size,
            "controlBytes" to decodeBase64OrEmpty(parsed.controlBase64).size,
            "enrichmentBytes" to decodeBase64OrEmpty(parsed.enrichmentBase64).size,
        )
        startFieldService()
        useDeviceLocation = parsed.useDeviceLocation
        state.advertiseMode = parsed.advertiseMode
        state.error = null
        state.running = false
        state.advertising = false
        state.advertiseStartConfirmed = false

        cacheAndApply(PHONE_METADATA_UUID, parsed.metadata.toByteArray(StandardCharsets.UTF_8), true)
        cacheAndApply(PHONE_GPS_UUID, decodeBase64OrEmpty(parsed.gpsBase64), true)
        cacheAndApply(PHONE_CONTROL_UUID, decodeBase64OrEmpty(parsed.controlBase64), true)
        cacheAndApply(PHONE_ENRICHMENT_UUID, decodeBase64OrEmpty(parsed.enrichmentBase64), true)
        cacheAndApply(PHONE_COMMAND_REQ_UUID, commandChannel.requestBytes(), true)
        // AUTH starts empty; the device populates it via a write challenge,
        // and the session response gets installed by handleIncomingAuthChallenge.
        cacheAndApply(PHONE_AUTH_UUID, ByteArray(0), false)
        secureSession.reset()
        state.secureSessionReady = false

        val adapter = bluetoothAdapter
        if (adapter == null || !adapter.isEnabled) {
          traceWarn("start_blocked", "reason" to "adapter_unavailable_or_disabled")
          failStartup("Bluetooth adapter is unavailable or disabled.", null)
          promise.resolve(currentStateMap())
          return@runOnMain
        }

        val server = bluetoothManager.openGattServer(reactContext, gattCallback)
        if (server == null) {
          traceWarn("start_blocked", "reason" to "open_gatt_server_failed")
          failStartup("Unable to open Android GATT server.", null)
          promise.resolve(currentStateMap())
          return@runOnMain
        }

        bluetoothGattServer = server
        val builtService = buildGattService()
        service = builtService
        if (!server.addService(builtService)) {
          traceWarn("start_blocked", "reason" to "add_service_returned_false")
          failStartup("Unable to add Spectre GATT service.", null)
          promise.resolve(currentStateMap())
          return@runOnMain
        }

        bluetoothAdvertiser = adapter.bluetoothLeAdvertiser
        if (bluetoothAdvertiser == null) {
          traceWarn("start_blocked", "reason" to "advertiser_unavailable")
          failStartup("Bluetooth LE advertising is unavailable on this device.", null)
          promise.resolve(currentStateMap())
          return@runOnMain
        }

        startAdvertising(parsed.advertiseMode)
        state.running = true
        state.advertising = advertiseCallback != null
        state.watchdogActive = state.advertising
        armAdvertisingWatchdog()
        // Session is established lazily when the device writes its challenge
        // to the AUTH characteristic, not at startup.
        state.secureSessionReady = false
        state.lastAdvertiseStartedAt = System.currentTimeMillis()
        emitState()
        promise.resolve(currentStateMap())
      } catch (error: Exception) {
        traceError(
            "start_exception",
            "type" to error.javaClass.simpleName,
            "message" to error.message,
        )
        failStartup(error.message ?: "Failed to start Spectre peripheral.", error)
        promise.resolve(currentStateMap())
      }
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun stopServer(promise: Promise) {
    runOnMain {
      traceInfo("stop_server", "running" to state.running, "connected" to connectedDevices.size)
      stopInternal()
      promise.resolve(currentStateMap())
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun updateMetadata(metadata: String, promise: Promise) {
    runOnMain {
      traceInfo("update_value", "char" to uuidLabel(PHONE_METADATA_UUID), "bytes" to metadata.toByteArray(StandardCharsets.UTF_8).size)
      cacheAndApply(PHONE_METADATA_UUID, metadata.toByteArray(StandardCharsets.UTF_8), true)
      promise.resolve(null)
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun updateGpsValue(gpsBase64: String, promise: Promise) {
    runOnMain {
      val bytes = decodeBase64OrEmpty(gpsBase64)
      traceInfo("update_value", "char" to uuidLabel(PHONE_GPS_UUID), "bytes" to bytes.size)
      cacheAndApply(PHONE_GPS_UUID, bytes, true)
      promise.resolve(null)
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun updateControlValue(controlBase64: String, promise: Promise) {
    runOnMain {
      val bytes = decodeBase64OrEmpty(controlBase64)
      traceInfo("update_value", "char" to uuidLabel(PHONE_CONTROL_UUID), "bytes" to bytes.size)
      cacheAndApply(PHONE_CONTROL_UUID, bytes, true)
      promise.resolve(null)
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun updateEnrichmentValue(enrichmentBase64: String, notify: Boolean, promise: Promise) {
    runOnMain {
      val bytes = decodeBase64OrEmpty(enrichmentBase64)
      traceInfo(
          "update_value",
          "char" to uuidLabel(PHONE_ENRICHMENT_UUID),
          "bytes" to bytes.size,
          "notify" to notify,
      )
      cacheAndApply(PHONE_ENRICHMENT_UUID, bytes, notify)
      promise.resolve(null)
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun updateCommandRequestValue(requestBase64: String, promise: Promise) {
    runOnMain {
      val plaintext = decodeBase64OrEmpty(requestBase64)
      if (!secureSession.ready) {
        traceWarn("command_send_blocked", "reason" to "secure_session_not_ready", "bytes" to plaintext.size)
        promise.reject(
            "E_SECURE_SESSION_NOT_READY",
            "Cannot send command before secure session is established",
        )
        return@runOnMain
      }
      val envelope = secureSession.encrypt(PHONE_SECURE_CHANNEL_COMMAND, plaintext)
      if (envelope == null) {
        traceWarn(
            "command_encrypt_failed",
            "reason" to (secureSession.lastError ?: "unknown"),
            "bytes" to plaintext.size,
        )
        promise.reject(
            "E_COMMAND_ENCRYPT_FAILED",
            secureSession.lastError ?: "encrypt returned null",
        )
        return@runOnMain
      }
      commandChannel.updateRequest(plaintext)
      traceInfo("command_send", "plainBytes" to plaintext.size, "envelopeBytes" to envelope.size)
      cacheAndApply(PHONE_COMMAND_REQ_UUID, envelope, true)
      promise.resolve(null)
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun getLastKnownLocation(promise: Promise) {
    runOnMain {
      try {
        val location = readLastKnownLocation()
        if (location == null) {
          promise.resolve(null)
        } else {
          promise.resolve(locationToMap(location))
        }
      } catch (error: SecurityException) {
        promise.resolve(null)
      }
    }
  }

  @com.facebook.react.bridge.ReactMethod
  fun getAuthPublicKey(promise: Promise) {
    runOnMain {
      val hex = secureSession.phonePublicKeyHex()
      if (hex == null) {
        promise.reject(
            "E_AUTH_KEY",
            secureSession.lastError ?: "phone public key unavailable",
        )
        return@runOnMain
      }
      promise.resolve(hex)
    }
  }

  private val gattCallback =
      object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
          runOnMain {
            val peer = describeDevice(device)
            traceInfo(
                "connection_state",
                "peer" to describeDeviceForLog(device),
                "status" to status,
                "newState" to bluetoothStateName(newState),
                "connectedBefore" to connectedDevices.size,
            )
            if (newState == BluetoothProfile.STATE_CONNECTED) {
              connectedDevices[device.address] = device
              state.connectedDevices = connectedDevices.size
              state.lastConnectedAt = System.currentTimeMillis()
              state.lastConnectedPeer = peer
              emitLog("Peripheral connected: $peer")
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
              connectedDevices.remove(device.address)
              state.connectedDevices = connectedDevices.size
              state.lastDisconnectedAt = System.currentTimeMillis()
              state.lastDisconnectedPeer = peer
              // Tear down session state so the next reconnect re-handshakes
              // (counters reset, keys cleared).  Each session is per-link.
              if (connectedDevices.isEmpty()) {
                secureSession.reset()
                state.secureSessionReady = false
                restartAdvertising("disconnect")
              }
              emitLog("Peripheral disconnected: $peer")
            }
            state.watchdogActive = state.running && state.advertising
            emitState()
          }
        }

        override fun onServiceAdded(status: Int, service: BluetoothGattService) {
          runOnMain {
            traceInfo(
                "service_added",
                "status" to gattStatusName(status),
                "uuid" to uuidLabel(service.uuid),
                "characteristics" to service.characteristics.size,
            )
            if (status == BluetoothGatt.GATT_SUCCESS) {
              state.advertiseStartConfirmed = true
              emitLog("Spectre GATT service ready")
            } else {
              state.lastAdvertiseFailureCode = status
              state.error = "Failed to add GATT service (status=$status)"
              emitLog(state.error ?: "Failed to add GATT service")
            }
            emitState()
          }
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice,
            requestId: Int,
            offset: Int,
            characteristic: BluetoothGattCharacteristic,
        ) {
          val value = characteristicCache[characteristic.uuid] ?: ByteArray(0)
          val slice =
              if (offset in 0..value.size) value.copyOfRange(offset, value.size) else ByteArray(0)
          traceInfo(
              "char_read",
              "peer" to describeDeviceForLog(device),
              "char" to uuidLabel(characteristic.uuid),
              "requestId" to requestId,
              "offset" to offset,
              "valueBytes" to value.size,
              "responseBytes" to slice.size,
          )
          bluetoothGattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, slice)
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
          runOnMain {
            val merged = mergeWrite(characteristic.uuid, offset, value)
            characteristicCache[characteristic.uuid] = merged
            characteristic.setValue(merged)
            traceInfo(
                "char_write",
                "peer" to describeDeviceForLog(device),
                "char" to uuidLabel(characteristic.uuid),
                "requestId" to requestId,
                "offset" to offset,
                "chunkBytes" to value.size,
                "totalBytes" to merged.size,
                "prepared" to preparedWrite,
                "responseNeeded" to responseNeeded,
            )

            when (characteristic.uuid) {
              PHONE_AUTH_UUID -> handleIncomingAuthChallenge(device, merged)
              PHONE_EVENT_BATCH_UUID -> handleIncomingEventBatch(device, merged)
              PHONE_STORAGE_UUID -> handleIncomingStorageSnapshot(device, merged)
              PHONE_COMMAND_RESP_UUID -> handleIncomingCommandResponse(device, merged)
              PHONE_LOG_STREAM_UUID -> handleIncomingLogStreamChunk(device, merged)
              PHONE_DASHBOARD_STREAM_UUID -> handleIncomingDashboardStreamChunk(device, merged)
              PHONE_NOTIFICATION_UUID -> handleIncomingNotification(device, merged)
              else -> Unit
            }

            if (responseNeeded) {
              bluetoothGattServer?.sendResponse(
                  device,
                  requestId,
                  BluetoothGatt.GATT_SUCCESS,
                  offset,
                  null,
              )
            }
          }
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice,
            requestId: Int,
            descriptor: BluetoothGattDescriptor,
            preparedWrite: Boolean,
            responseNeeded: Boolean,
            offset: Int,
            value: ByteArray,
        ) {
          descriptor.setValue(value)
          traceInfo(
              "descriptor_write",
              "peer" to describeDeviceForLog(device),
              "descriptor" to uuidLabel(descriptor.uuid),
              "char" to uuidLabel(descriptor.characteristic.uuid),
              "requestId" to requestId,
              "offset" to offset,
              "bytes" to value.size,
              "responseNeeded" to responseNeeded,
              "value" to cccdValueLabel(value),
          )
          if (responseNeeded) {
            bluetoothGattServer?.sendResponse(
                device,
                requestId,
                BluetoothGatt.GATT_SUCCESS,
                offset,
                null,
            )
          }
        }
      }

  private fun parseStartConfig(config: ReadableMap): StartConfig {
    val metadata = config.getString("metadata") ?: ""
    val gpsBase64 = config.getString("gpsBase64") ?: ""
    val controlBase64 = config.getString("controlBase64") ?: ""
    val enrichmentBase64 = config.getString("enrichmentBase64") ?: ""
    val advertiseMode = config.getString("advertiseMode") ?: "service"
    val useDeviceLocation = config.hasKey("useDeviceLocation") && config.getBoolean("useDeviceLocation")

    return StartConfig(
        metadata = metadata,
        gpsBase64 = gpsBase64,
        controlBase64 = controlBase64,
        enrichmentBase64 = enrichmentBase64,
        advertiseMode = advertiseMode,
        useDeviceLocation = useDeviceLocation,
    )
  }

  private fun buildGattService(): BluetoothGattService {
    val builtService = BluetoothGattService(PHONE_SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)

    addCharacteristic(
        builtService,
        PHONE_GPS_UUID,
        BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
        BluetoothGattCharacteristic.PERMISSION_READ,
    )
    addCharacteristic(
        builtService,
        PHONE_CONTROL_UUID,
        BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
        BluetoothGattCharacteristic.PERMISSION_READ,
    )
    addCharacteristic(
        builtService,
        PHONE_METADATA_UUID,
        BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
        BluetoothGattCharacteristic.PERMISSION_READ,
    )
    addCharacteristic(
        builtService,
        PHONE_EVENT_BATCH_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )
    addCharacteristic(
        builtService,
        PHONE_ENRICHMENT_UUID,
        BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
        BluetoothGattCharacteristic.PERMISSION_READ,
    )
    // AUTH: device writes the challenge, phone notifies its response.  Needs
    // WRITE (device → phone) plus NOTIFY (phone → device).  No READ — the
    // device never reads this characteristic, only writes + subscribes.
    addCharacteristic(
        builtService,
        PHONE_AUTH_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )
    addCharacteristic(
        builtService,
        PHONE_STORAGE_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )
    addCharacteristic(
        builtService,
        PHONE_COMMAND_REQ_UUID,
        BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
        BluetoothGattCharacteristic.PERMISSION_READ,
    )
    addCharacteristic(
        builtService,
        PHONE_COMMAND_RESP_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )
    addCharacteristic(
        builtService,
        PHONE_LOG_STREAM_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )
    addCharacteristic(
        builtService,
        PHONE_DASHBOARD_STREAM_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )
    addCharacteristic(
        builtService,
        PHONE_NOTIFICATION_UUID,
        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
        BluetoothGattCharacteristic.PERMISSION_WRITE,
    )

    return builtService
  }

  private fun addCharacteristic(
      service: BluetoothGattService,
      uuid: UUID,
      properties: Int,
      permissions: Int,
  ) {
    val characteristic = BluetoothGattCharacteristic(uuid, properties, permissions)
    val requiresNotify =
        properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0
    if (requiresNotify) {
      characteristic.addDescriptor(
          BluetoothGattDescriptor(
              CLIENT_CONFIG_UUID,
              BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE,
          ).apply {
            value = BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
          }
      )
    }
    service.addCharacteristic(characteristic)
    characteristicCache[uuid] = characteristic.getValue() ?: ByteArray(0)
    traceInfo(
        "service_char_added",
        "char" to uuidLabel(uuid),
        "properties" to characteristicPropertiesLabel(properties),
        "permissions" to permissions,
    )
  }

  private fun startAdvertising(mode: String) {
    val advertiser = bluetoothAdvertiser ?: return
    stopAdvertisingOnly()
    state.advertiseMode = mode
    val serviceUuid = ParcelUuid(PHONE_SERVICE_UUID)
    val includeName = mode != "uuidOnly"
    val includeService = mode != "nameOnly" && mode != "shortName"
    val dataBuilder = AdvertiseData.Builder().setIncludeDeviceName(includeName)
    if (includeService || mode == "service" || mode == "uuidOnly") {
      dataBuilder.addServiceUuid(serviceUuid)
    }
    traceInfo(
        "advertise_start_request",
        "mode" to mode,
        "includeName" to includeName,
        "includeService" to (includeService || mode == "service" || mode == "uuidOnly"),
        "connectable" to true,
    )

    val settings =
        AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .setConnectable(true)
            .setTimeout(0)
            .build()

    advertiseCallback =
        object : AdvertiseCallback() {
          override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
            runOnMain {
              state.advertising = true
              state.advertiseStartConfirmed = true
              state.lastAdvertiseStartedAt = System.currentTimeMillis()
              state.error = null
              state.watchdogActive = true
              traceInfo(
                  "advertise_start_success",
                  "mode" to (state.advertiseMode ?: mode),
                  "txPower" to settingsInEffect.txPowerLevel,
                  "advertiseMode" to settingsInEffect.mode,
                  "restarts" to state.totalAdvertiseRestarts,
              )
              emitLog("Spectre peripheral advertising started")
              emitState()
            }
          }

          override fun onStartFailure(errorCode: Int) {
            runOnMain {
              state.advertising = false
              state.lastAdvertiseFailureCode = errorCode
              state.error = "Advertising failed (code=$errorCode)"
              traceWarn(
                  "advertise_start_failure",
                  "code" to errorCode,
                  "reason" to advertiseFailureName(errorCode),
                  "mode" to (state.advertiseMode ?: mode),
              )
              emitLog(state.error ?: "Advertising failed")
              emitState()
            }
          }
        }

    try {
      advertiser.startAdvertising(settings, dataBuilder.build(), advertiseCallback)
    } catch (error: SecurityException) {
      traceError("advertise_start_exception", "type" to "SecurityException", "message" to error.message)
      failStartup("Missing Bluetooth advertise permission.", error)
    } catch (error: IllegalArgumentException) {
      state.lastAdvertiseFailureCode = ADVERTISE_ERROR_BAD_PAYLOAD
      traceError("advertise_start_exception", "type" to "IllegalArgumentException", "message" to error.message)
      failStartup("Invalid Bluetooth advertise payload.", error)
    }
  }

  private fun armAdvertisingWatchdog() {
    handler.removeCallbacks(advertiseWatchdog)
    if (state.running) {
      state.watchdogActive = true
      handler.postDelayed(advertiseWatchdog, ADVERTISE_WATCHDOG_INTERVAL_MS)
    }
  }

  private fun disarmAdvertisingWatchdog() {
    handler.removeCallbacks(advertiseWatchdog)
    state.watchdogActive = false
  }

  private fun restartAdvertising(reason: String) {
    if (!state.running || bluetoothAdvertiser == null) {
      traceInfo(
          "advertise_restart_ignored",
          "reason" to reason,
          "running" to state.running,
          "advertiserPresent" to (bluetoothAdvertiser != null),
      )
      return
    }
    val mode = state.advertiseMode ?: "uuidOnly"
    state.totalAdvertiseRestarts += 1
    traceInfo("advertise_restart", "reason" to reason, "mode" to mode, "count" to state.totalAdvertiseRestarts)
    emitLog("Advertising restart requested: $reason")
    startAdvertising(mode)
    emitState()
  }

  private fun kickAdvertisingFromServiceInternal() {
    if (!state.running) {
      return
    }
    armAdvertisingWatchdog()
    if (advertiseCallback == null || !state.advertising) {
      restartAdvertising("field_service")
    }
  }

  private fun stopAdvertisingOnly() {
    bluetoothAdvertiser?.let { advertiser ->
      advertiseCallback?.let { callback ->
        traceInfo("advertise_stop_request", "mode" to state.advertiseMode)
        runCatching { advertiser.stopAdvertising(callback) }
            .onFailure { traceWarn("advertise_stop_failed", "message" to it.message) }
      }
    }
    advertiseCallback = null
    state.advertising = false
    state.advertiseStartConfirmed = false
  }

  private fun stopInternal() {
    traceInfo(
        "stop_internal",
        "running" to state.running,
        "advertising" to state.advertising,
        "connected" to connectedDevices.size,
    )
    disarmAdvertisingWatchdog()
    stopFieldService()
    stopAdvertisingOnly()
    bluetoothGattServer?.close()
    bluetoothGattServer = null
    service = null
    commandChannel.clear()
    connectedDevices.clear()
    state.running = false
    state.advertising = false
    state.connectedDevices = 0
    state.error = null
    state.advertiseStartConfirmed = false
    emitState()
  }

  private fun stopFromNotificationInternal() {
    stopInternal()
    emitLog("Field Mode stopped from notification")
  }

  private fun failStartup(message: String, error: Throwable?) {
    traceWarn(
        "startup_failed",
        "message" to message,
        "errorType" to error?.javaClass?.simpleName,
        "error" to error?.message,
    )
    disarmAdvertisingWatchdog()
    stopFieldService()
    stopAdvertisingOnly()
    bluetoothGattServer?.close()
    bluetoothGattServer = null
    service = null
    connectedDevices.clear()
    state.running = false
    state.advertising = false
    state.error = message
    state.lastAdvertiseFailureCode = state.lastAdvertiseFailureCode ?: -1
    if (error != null) {
      emitLog("$message: ${error.message}")
    } else {
      emitLog(message)
    }
    emitState()
  }

  private fun startFieldService() {
    val intent = Intent(reactContext, SpectreFieldService::class.java)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      reactContext.startForegroundService(intent)
    } else {
      reactContext.startService(intent)
    }
  }

  private fun stopFieldService() {
    runCatching {
      reactContext.stopService(Intent(reactContext, SpectreFieldService::class.java))
    }
  }

  private fun handleIncomingEventBatch(device: BluetoothDevice, bytes: ByteArray) {
    val now = System.currentTimeMillis()
    val base64 = Base64.encodeToString(bytes, Base64.NO_WRAP)
    state.lastBatchReceivedAt = now
    state.lastBatchPeer = describeDevice(device)
    state.lastBatchBytes = bytes.size
    state.lastBatchRecords = bytes.size / 10
    state.totalBatchesReceived += 1
    state.totalBatchBytes += bytes.size.toLong()
    state.totalBatchRecords += state.lastBatchRecords.toLong()
    traceInfo(
        "event_batch_received",
        "peer" to describeDeviceForLog(device),
        "bytes" to bytes.size,
        "recordsEstimate" to state.lastBatchRecords,
        "totalBatches" to state.totalBatchesReceived,
    )
    emitEvent(
        "SpectrePeripheralEventBatch",
        Arguments.createMap().apply {
          putString("base64", base64)
          putDouble("length", bytes.size.toDouble())
          putDouble("receivedAt", now.toDouble())
        },
    )
    emitState()
  }

  private fun handleIncomingStorageSnapshot(device: BluetoothDevice, bytes: ByteArray) {
    val now = System.currentTimeMillis()
    val base64 = Base64.encodeToString(bytes, Base64.NO_WRAP)
    state.lastStorageReceivedAt = now
    state.lastStoragePeer = describeDevice(device)
    state.storageBase64 = base64
    traceInfo("storage_snapshot_received", "peer" to describeDeviceForLog(device), "bytes" to bytes.size)
    emitEvent(
        "SpectrePeripheralStorage",
        Arguments.createMap().apply {
          putString("base64", base64)
          putDouble("receivedAt", now.toDouble())
        },
    )
    emitState()
  }

  private fun handleIncomingCommandResponse(device: BluetoothDevice, bytes: ByteArray) {
    val now = System.currentTimeMillis()
    val plaintext = secureSession.decrypt(PHONE_SECURE_CHANNEL_COMMAND, bytes)
    if (plaintext == null) {
      traceWarn(
          "secure_decrypt_failed",
          "channel" to "command",
          "bytes" to bytes.size,
          "reason" to (secureSession.lastError ?: "unknown"),
      )
      emitLog("Command response decrypt failed: ${secureSession.lastError ?: "unknown"}")
      return
    }
    traceInfo("command_response_received", "peer" to describeDeviceForLog(device), "cipherBytes" to bytes.size, "plainBytes" to plaintext.size)
    val base64 = Base64.encodeToString(plaintext, Base64.NO_WRAP)
    commandChannel.updateResponse(plaintext)
    emitEvent(
        "SpectrePeripheralCommandResponse",
        Arguments.createMap().apply {
          putString("base64", base64)
          putDouble("receivedAt", now.toDouble())
        },
    )
  }

  private fun handleIncomingLogStreamChunk(device: BluetoothDevice, bytes: ByteArray) {
    val now = System.currentTimeMillis()
    val plaintext = secureSession.decrypt(PHONE_SECURE_CHANNEL_LOG_STREAM, bytes)
    if (plaintext == null) {
      traceWarn(
          "secure_decrypt_failed",
          "channel" to "log_stream",
          "bytes" to bytes.size,
          "reason" to (secureSession.lastError ?: "unknown"),
      )
      emitLog("Log stream decrypt failed: ${secureSession.lastError ?: "unknown"}")
      return
    }
    traceInfo("log_stream_received", "peer" to describeDeviceForLog(device), "cipherBytes" to bytes.size, "plainBytes" to plaintext.size)
    val base64 = Base64.encodeToString(plaintext, Base64.NO_WRAP)
    emitEvent(
        "SpectrePeripheralLogStream",
        Arguments.createMap().apply {
          putString("base64", base64)
          putDouble("receivedAt", now.toDouble())
        },
    )
  }

  private fun handleIncomingDashboardStreamChunk(device: BluetoothDevice, bytes: ByteArray) {
    val now = System.currentTimeMillis()
    val plaintext = secureSession.decrypt(PHONE_SECURE_CHANNEL_DASHBOARD_STREAM, bytes)
    if (plaintext == null) {
      traceWarn(
          "secure_decrypt_failed",
          "channel" to "dashboard_stream",
          "bytes" to bytes.size,
          "reason" to (secureSession.lastError ?: "unknown"),
      )
      emitLog("Dashboard stream decrypt failed: ${secureSession.lastError ?: "unknown"}")
      return
    }
    traceInfo("dashboard_stream_received", "peer" to describeDeviceForLog(device), "cipherBytes" to bytes.size, "plainBytes" to plaintext.size)
    val base64 = Base64.encodeToString(plaintext, Base64.NO_WRAP)
    emitEvent(
        "SpectrePeripheralDashboardStream",
        Arguments.createMap().apply {
          putString("base64", base64)
          putDouble("receivedAt", now.toDouble())
        },
    )
  }

  private fun handleIncomingNotification(device: BluetoothDevice, bytes: ByteArray) {
    val now = System.currentTimeMillis()
    val plaintext = secureSession.decrypt(PHONE_SECURE_CHANNEL_NOTIFICATION, bytes)
    if (plaintext == null) {
      traceWarn(
          "secure_decrypt_failed",
          "channel" to "notification",
          "bytes" to bytes.size,
          "reason" to (secureSession.lastError ?: "unknown"),
      )
      emitLog("Notification decrypt failed: ${secureSession.lastError ?: "unknown"}")
      return
    }
    traceInfo("notification_received", "peer" to describeDeviceForLog(device), "cipherBytes" to bytes.size, "plainBytes" to plaintext.size)
    val base64 = Base64.encodeToString(plaintext, Base64.NO_WRAP)
    emitEvent(
        "SpectrePeripheralNotification",
        Arguments.createMap().apply {
          putString("base64", base64)
          putDouble("receivedAt", now.toDouble())
        },
    )
  }

  private fun handleIncomingAuthChallenge(device: BluetoothDevice, bytes: ByteArray) {
    traceInfo("auth_challenge_received", "peer" to describeDeviceForLog(device), "bytes" to bytes.size)
    val response = secureSession.handleChallenge(bytes)
    if (response == null) {
      traceWarn(
          "auth_challenge_rejected",
          "peer" to describeDeviceForLog(device),
          "bytes" to bytes.size,
          "reason" to (secureSession.lastError ?: "unknown"),
      )
      emitLog("Auth challenge rejected: ${secureSession.lastError ?: "unknown"}")
      state.secureSessionReady = false
      emitState()
      return
    }
    // Publish the response on the AUTH characteristic and notify; the device
    // is subscribed and consumes it via its auth notify callback.
    cacheAndApply(PHONE_AUTH_UUID, response, true)
    state.secureSessionReady = true
    traceInfo("auth_session_ready", "peer" to describeDeviceForLog(device), "responseBytes" to response.size)
    emitLog("Secure session established with ${describeDevice(device)}")
    emitState()
  }

  private fun cacheAndApply(uuid: UUID, bytes: ByteArray, notify: Boolean) {
    characteristicCache[uuid] = bytes.copyOf()
    service?.getCharacteristic(uuid)?.apply {
      value = bytes.copyOf()
      if (notify) {
        traceInfo(
            "notify_requested",
            "char" to uuidLabel(uuid),
            "bytes" to bytes.size,
            "connected" to connectedDevices.size,
        )
        notifyAllDevices(this)
      }
    }
    if (uuid == PHONE_COMMAND_REQ_UUID) {
      commandChannel.updateRequest(bytes)
    }
    emitState()
  }

  private fun notifyAllDevices(characteristic: BluetoothGattCharacteristic) {
    val server = bluetoothGattServer ?: return
    if (connectedDevices.isEmpty()) {
      traceInfo("notify_skipped", "char" to uuidLabel(characteristic.uuid), "reason" to "no_connected_devices")
      return
    }

    for (device in connectedDevices.values) {
      traceInfo(
          "notify_device",
          "peer" to describeDeviceForLog(device),
          "char" to uuidLabel(characteristic.uuid),
          "bytes" to (characteristicCache[characteristic.uuid]?.size ?: 0),
      )
      notifyCharacteristicChanged(
          server,
          device,
          characteristic,
          characteristicCache[characteristic.uuid] ?: ByteArray(0),
      )
    }
  }

  @Suppress("DEPRECATION")
  private fun notifyCharacteristicChanged(
      server: BluetoothGattServer,
      device: BluetoothDevice,
      characteristic: BluetoothGattCharacteristic,
      value: ByteArray,
  ) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      server.notifyCharacteristicChanged(device, characteristic, false, value)
    } else {
      server.notifyCharacteristicChanged(device, characteristic, false)
    }
  }

  private fun mergeWrite(uuid: UUID, offset: Int, value: ByteArray): ByteArray {
    val existing = characteristicCache[uuid] ?: ByteArray(0)
    if (offset <= 0) {
      return value.copyOf()
    }

    val size = maxOf(existing.size, offset + value.size)
    val merged = ByteArray(size)
    System.arraycopy(existing, 0, merged, 0, existing.size)
    System.arraycopy(value, 0, merged, offset, value.size)
    return merged
  }

  private fun readLastKnownLocation(): Location? {
    val manager = reactContext.getSystemService(Context.LOCATION_SERVICE) as LocationManager
    val providers = listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER, LocationManager.PASSIVE_PROVIDER)
    for (provider in providers) {
      try {
        if (!manager.isProviderEnabled(provider)) {
          continue
        }
      } catch (_: SecurityException) {
        continue
      }
      val location = try {
        manager.getLastKnownLocation(provider)
      } catch (error: SecurityException) {
        null
      }
      if (location != null) {
        return location
      }
    }
    return null
  }

  // Legacy RSA Keystore key generation removed — phone identity is now the
  // P-256 keypair owned by [BleSecureSession] (see SpectreSecrets).

  private fun locationToMap(location: Location): WritableMap {
    return Arguments.createMap().apply {
      putDouble("lat", location.latitude)
      putDouble("lon", location.longitude)
      putDouble("alt", if (location.hasAltitude()) location.altitude else 0.0)
      putDouble("accuracy", if (location.hasAccuracy()) location.accuracy.toDouble() else 0.0)
      putDouble("timestamp", location.time.toDouble())
      putString("provider", location.provider)
    }
  }

  private fun currentStateMap(): WritableMap {
    return Arguments.createMap().apply {
      putBoolean("running", state.running)
      putBoolean("advertising", state.advertising)
      putDouble("connectedDevices", state.connectedDevices.toDouble())
      putBoolean("secureSessionReady", state.secureSessionReady)
      putString("advertiseMode", state.advertiseMode)
      putBoolean("advertiseStartConfirmed", state.advertiseStartConfirmed)
      putBoolean("watchdogActive", state.watchdogActive)
      putDouble("totalAdvertiseRestarts", state.totalAdvertiseRestarts.toDouble())
      putNullableLong("lastAdvertiseStartedAt", state.lastAdvertiseStartedAt)
      putNullableLong("lastAdvertiseFailureCode", state.lastAdvertiseFailureCode?.toLong())
      putNullableLong("lastConnectedAt", state.lastConnectedAt)
      putNullableLong("lastDisconnectedAt", state.lastDisconnectedAt)
      putString("lastConnectedPeer", state.lastConnectedPeer)
      putString("lastDisconnectedPeer", state.lastDisconnectedPeer)
      putNullableLong("lastBatchReceivedAt", state.lastBatchReceivedAt)
      putString("lastBatchPeer", state.lastBatchPeer)
      putDouble("lastBatchBytes", state.lastBatchBytes.toDouble())
      putDouble("lastBatchRecords", state.lastBatchRecords.toDouble())
      putNullableLong("lastStorageReceivedAt", state.lastStorageReceivedAt)
      putString("lastStoragePeer", state.lastStoragePeer)
      putString("storageBase64", state.storageBase64)
      putDouble("totalBatchesReceived", state.totalBatchesReceived.toDouble())
      putDouble("totalBatchBytes", state.totalBatchBytes.toDouble())
      putDouble("totalBatchRecords", state.totalBatchRecords.toDouble())
      putString("error", state.error)
      putBoolean("moduleAvailable", state.moduleAvailable)
    }
  }

  private fun emitState() {
    emitEvent("SpectrePeripheralState", currentStateMap())
  }

  private fun WritableMap.putNullableLong(key: String, value: Long?) {
    if (value == null) {
      putNull(key)
    } else {
      putDouble(key, value.toDouble())
    }
  }

  private fun emitLog(message: String) {
    traceInfo("app_log", "message" to message)
    emitEvent(
        "SpectrePeripheralLog",
        Arguments.createMap().apply {
          putString("message", message)
        },
    )
  }

  private fun emitEvent(eventName: String, payload: WritableMap) {
    if (!reactContext.hasActiveReactInstance()) {
      return
    }
    reactContext
        .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
        .emit(eventName, payload)
  }

  private fun describeDevice(device: BluetoothDevice): String {
    return device.name?.takeIf { it.isNotBlank() } ?: device.address
  }

  private fun describeDeviceForLog(device: BluetoothDevice): String {
    val name = device.name?.takeIf { it.isNotBlank() }
    val suffix = device.address?.takeLast(5) ?: "unknown"
    return if (name == null) "xx:$suffix" else "$name(xx:$suffix)"
  }

  private fun traceInfo(event: String, vararg fields: Pair<String, Any?>) {
    trace(Log.INFO, event, *fields)
  }

  private fun traceWarn(event: String, vararg fields: Pair<String, Any?>) {
    trace(Log.WARN, event, *fields)
  }

  private fun traceError(event: String, vararg fields: Pair<String, Any?>) {
    trace(Log.ERROR, event, *fields)
  }

  private fun trace(level: Int, event: String, vararg fields: Pair<String, Any?>) {
    val message =
        buildString {
          append("event=").append(event)
          fields.forEach { (key, value) ->
            append(' ')
            append(key)
            append('=')
            append(formatLogValue(value))
          }
        }
    Log.println(level, LOG_TAG, message)
  }

  private fun formatLogValue(value: Any?): String {
    val raw =
        when (value) {
          null -> "null"
          is UUID -> uuidLabel(value)
          else -> value.toString()
        }
    val cleaned =
        raw
            .replace('\n', ' ')
            .replace('\r', ' ')
            .replace('\t', ' ')
            .trim()
    return if (cleaned.isBlank()) "\"\"" else cleaned
  }

  private fun uuidLabel(uuid: UUID): String =
      when (uuid) {
        PHONE_SERVICE_UUID -> "phone_service"
        PHONE_GPS_UUID -> "gps"
        PHONE_CONTROL_UUID -> "control"
        PHONE_METADATA_UUID -> "metadata"
        PHONE_EVENT_BATCH_UUID -> "event_batch"
        PHONE_ENRICHMENT_UUID -> "enrichment"
        PHONE_AUTH_UUID -> "auth"
        PHONE_STORAGE_UUID -> "storage"
        PHONE_COMMAND_REQ_UUID -> "command_req"
        PHONE_COMMAND_RESP_UUID -> "command_resp"
        PHONE_LOG_STREAM_UUID -> "log_stream"
        PHONE_DASHBOARD_STREAM_UUID -> "dashboard_stream"
        PHONE_NOTIFICATION_UUID -> "notification"
        CLIENT_CONFIG_UUID -> "cccd"
        else -> uuid.toString()
      }

  private fun bluetoothStateName(state: Int): String =
      when (state) {
        BluetoothProfile.STATE_CONNECTED -> "connected"
        BluetoothProfile.STATE_CONNECTING -> "connecting"
        BluetoothProfile.STATE_DISCONNECTED -> "disconnected"
        BluetoothProfile.STATE_DISCONNECTING -> "disconnecting"
        else -> state.toString()
      }

  private fun gattStatusName(status: Int): String =
      if (status == BluetoothGatt.GATT_SUCCESS) "success" else status.toString()

  private fun advertiseFailureName(errorCode: Int): String =
      when (errorCode) {
        AdvertiseCallback.ADVERTISE_FAILED_ALREADY_STARTED -> "already_started"
        AdvertiseCallback.ADVERTISE_FAILED_DATA_TOO_LARGE -> "data_too_large"
        AdvertiseCallback.ADVERTISE_FAILED_FEATURE_UNSUPPORTED -> "feature_unsupported"
        AdvertiseCallback.ADVERTISE_FAILED_INTERNAL_ERROR -> "internal_error"
        AdvertiseCallback.ADVERTISE_FAILED_TOO_MANY_ADVERTISERS -> "too_many_advertisers"
        ADVERTISE_ERROR_BAD_PAYLOAD -> "bad_payload"
        else -> "unknown"
      }

  private fun cccdValueLabel(value: ByteArray): String =
      when {
        value.contentEquals(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) -> "notify"
        value.contentEquals(BluetoothGattDescriptor.ENABLE_INDICATION_VALUE) -> "indicate"
        value.contentEquals(BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE) -> "disable"
        else -> "bytes:${value.size}"
      }

  private fun characteristicPropertiesLabel(properties: Int): String {
    val labels = mutableListOf<String>()
    if (properties and BluetoothGattCharacteristic.PROPERTY_READ != 0) labels.add("read")
    if (properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) labels.add("write")
    if (properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) labels.add("write_no_response")
    if (properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) labels.add("notify")
    if (properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0) labels.add("indicate")
    return if (labels.isEmpty()) properties.toString() else labels.joinToString("|")
  }

  private fun decodeBase64OrEmpty(value: String): ByteArray {
    if (value.isBlank()) {
      return ByteArray(0)
    }
    return try {
      Base64.decode(value, Base64.DEFAULT)
    } catch (_: IllegalArgumentException) {
      ByteArray(0)
    }
  }

  private fun runOnMain(block: () -> Unit) {
    if (Looper.myLooper() == Looper.getMainLooper()) {
      block()
    } else {
      handler.post(block)
    }
  }

  companion object {
    @Volatile
    private var activeModule: WeakReference<SpectrePeripheralModule>? = null

    fun stopFromNotification() {
      val module = activeModule?.get()
      module?.runOnMain {
        module.stopFromNotificationInternal()
      }
    }

    fun kickFromFieldService() {
      val module = activeModule?.get()
      module?.runOnMain {
        module.kickAdvertisingFromServiceInternal()
      }
    }

    // Mirrors PHONE_SECURE_CHANNEL_* in src/protocol/CompanionProtocol.h.
    private const val PHONE_SECURE_CHANNEL_COMMAND = 0x07
    private const val PHONE_SECURE_CHANNEL_LOG_STREAM = 0x08
    private const val PHONE_SECURE_CHANNEL_DASHBOARD_STREAM = 0x09
    private const val PHONE_SECURE_CHANNEL_NOTIFICATION = 0x0a
    private const val ADVERTISE_WATCHDOG_INTERVAL_MS = 5_000L
    private const val ADVERTISE_KICK_INTERVAL_MS = 12_000L
    private const val ADVERTISE_ERROR_BAD_PAYLOAD = -2
    private const val LOG_TAG = "SpectrePeripheral"

    private val PHONE_SERVICE_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0001")
    private val PHONE_GPS_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0002")
    private val PHONE_CONTROL_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0003")
    private val PHONE_METADATA_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0004")
    private val PHONE_EVENT_BATCH_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0005")
    private val PHONE_ENRICHMENT_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0006")
    private val PHONE_AUTH_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0007")
    private val PHONE_STORAGE_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0008")
    private val PHONE_COMMAND_REQ_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a0009")
    private val PHONE_COMMAND_RESP_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a000a")
    private val PHONE_LOG_STREAM_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a000b")
    private val PHONE_DASHBOARD_STREAM_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a000c")
    private val PHONE_NOTIFICATION_UUID = UUID.fromString("84f03a80-6d7b-4d4d-9a64-6b2d6f3a000d")
    private val CLIENT_CONFIG_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
  }
}
