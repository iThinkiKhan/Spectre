package com.spectre.companion

import android.util.Base64

internal class SpectrePeripheralCommandChannel {
  private var requestBytes: ByteArray = ByteArray(0)
  private var responseBytes: ByteArray = ByteArray(0)

  fun updateRequest(bytes: ByteArray): Boolean {
    val changed = !requestBytes.contentEquals(bytes)
    requestBytes = bytes.copyOf()
    return changed
  }

  fun updateResponse(bytes: ByteArray): Boolean {
    val changed = !responseBytes.contentEquals(bytes)
    responseBytes = bytes.copyOf()
    return changed
  }

  fun clear() {
    requestBytes = ByteArray(0)
    responseBytes = ByteArray(0)
  }

  fun requestBytes(): ByteArray = requestBytes.copyOf()

  fun responseBytes(): ByteArray = responseBytes.copyOf()

  fun requestBase64(): String = Base64.encodeToString(requestBytes, Base64.NO_WRAP)

  fun responseBase64(): String = Base64.encodeToString(responseBytes, Base64.NO_WRAP)
}
