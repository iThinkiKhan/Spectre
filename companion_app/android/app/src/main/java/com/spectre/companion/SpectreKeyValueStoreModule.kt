package com.spectre.companion

import android.content.Context
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

class SpectreKeyValueStoreModule(
    reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext) {
  private val prefs =
      reactContext.getSharedPreferences("spectre_companion_store", Context.MODE_PRIVATE)

  override fun getName(): String = "SpectreKeyValueStore"

  @ReactMethod
  fun getString(key: String, promise: Promise) {
    promise.resolve(if (prefs.contains(key)) prefs.getString(key, null) else null)
  }

  @ReactMethod
  fun setString(key: String, value: String, promise: Promise) {
    prefs.edit().putString(key, value).apply()
    promise.resolve(true)
  }

  @ReactMethod
  fun remove(key: String, promise: Promise) {
    prefs.edit().remove(key).apply()
    promise.resolve(true)
  }
}
