package com.spectre.companion

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
import java.lang.ref.WeakReference
import org.json.JSONObject

/**
 * JS-facing bridge for [SpectreLocationService].
 *
 * The service itself owns the GPS loop and writes fixes directly to the same
 * SharedPreferences buckets that LocationHistoryStore.ts reads on startup, so
 * the recorder keeps working when JS is dead.  This module exists so JS can:
 *   - kick the service after permissions are granted ([start]),
 *   - stop it on opt-out ([stop]),
 *   - receive fixes in real-time while alive (via the `SpectreLocationFix`
 *     RN event, used to update the BLE GPS characteristic and the live
 *     in-memory enrichment buffer).
 */
class SpectreLocationRecorderModule(
    private val reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext) {

  init {
    activeModule = WeakReference(this)
  }

  override fun getName(): String = NAME

  override fun invalidate() {
    if (activeModule?.get() === this) {
      activeModule = null
    }
    super.invalidate()
  }

  @ReactMethod
  fun addListener(eventName: String?) = Unit

  @ReactMethod
  fun removeListeners(count: Double) = Unit

  @ReactMethod
  fun start(promise: Promise) {
    try {
      SpectreLocationService.start(reactContext.applicationContext)
      promise.resolve(true)
    } catch (error: Exception) {
      promise.reject("E_LOCATION_START_FAILED", error.message, error)
    }
  }

  @ReactMethod
  fun stop(promise: Promise) {
    try {
      SpectreLocationService.stop(reactContext.applicationContext)
      promise.resolve(true)
    } catch (error: Exception) {
      promise.reject("E_LOCATION_STOP_FAILED", error.message, error)
    }
  }

  private fun emitFix(fix: JSONObject) {
    if (!reactContext.hasActiveReactInstance()) return
    val payload: WritableMap =
        Arguments.createMap().apply {
          putDouble("lat", fix.optDouble("lat", 0.0))
          putDouble("lon", fix.optDouble("lon", 0.0))
          putDouble("alt", fix.optDouble("alt", 0.0))
          putDouble("accuracy", fix.optDouble("accuracy", 0.0))
          putDouble("timestamp", fix.optDouble("timestamp", 0.0))
          putString("source", fix.optString("source", "device"))
          putString("provider", fix.optString("provider", "fused"))
        }
    reactContext
        .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
        .emit("SpectreLocationFix", payload)
  }

  companion object {
    const val NAME = "SpectreLocationRecorder"

    @Volatile
    private var activeModule: WeakReference<SpectreLocationRecorderModule>? = null

    fun dispatchFix(fix: JSONObject) {
      activeModule?.get()?.emitFix(fix)
    }
  }
}
