package com.spectre.companion

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Legacy receiver kept harmless if an old install still has it registered.
 * New installs do not register it in AndroidManifest.xml.
 */
class SpectreBootReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent?) {
    val action = intent?.action ?: return
    Log.i(TAG, "event=boot_skip reason=field_mode_required action=$action")
  }

  companion object {
    private const val TAG = "SpectreBootReceiver"
  }
}
