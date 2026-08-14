@file:Suppress("DEPRECATION")

package com.spectre.companion

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

@Suppress("DEPRECATION")
class SpectreCompanionPackage : ReactPackage {
  @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
  override fun createNativeModules(
      reactContext: ReactApplicationContext,
  ): List<NativeModule> = listOf(
      SpectrePeripheralModule(reactContext),
      SpectreFileImportModule(reactContext),
      SpectreKeyValueStoreModule(reactContext),
      SpectreLocationRecorderModule(reactContext),
      SpectreRelayModule(reactContext),
      SpectreMapTileModule(reactContext),
  )

  override fun createViewManagers(
      reactContext: ReactApplicationContext,
  ): List<ViewManager<*, *>> = emptyList()
}
