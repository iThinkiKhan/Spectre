package com.spectre.companion

import android.app.Activity
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.provider.OpenableColumns
import com.facebook.react.bridge.ActivityEventListener
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReadableMap
import java.io.BufferedReader
import java.io.InputStreamReader
import java.nio.charset.StandardCharsets

class SpectreFileImportModule(
    reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext), ActivityEventListener {
  private var pendingPromise: Promise? = null

  init {
    reactContext.addActivityEventListener(this)
  }

  override fun getName(): String = "SpectreFileImport"

  override fun onActivityResult(
      activity: Activity,
      requestCode: Int,
      resultCode: Int,
      data: Intent?,
  ) {
    if (requestCode != REQUEST_CODE_PICK_TEXT) {
      return
    }

    val promise = pendingPromise ?: return
    pendingPromise = null

    if (resultCode != Activity.RESULT_OK) {
      promise.resolve(null)
      return
    }

    val uri = data?.data
    if (uri == null) {
      promise.resolve(null)
      return
    }

    try {
      promise.resolve(readImportedFile(uri))
    } catch (error: Exception) {
      promise.reject("E_FILE_IMPORT", error.message, error)
    }
  }

  override fun onNewIntent(intent: Intent) {
    // No-op.
  }

  @com.facebook.react.bridge.ReactMethod
  fun pickTextFile(promise: Promise) {
    if (pendingPromise != null) {
      promise.reject("E_FILE_IMPORT_BUSY", "A file picker request is already in flight.")
      return
    }

    val activity = reactApplicationContext.currentActivity
    if (activity == null) {
      promise.reject("E_FILE_IMPORT_NO_ACTIVITY", "Android activity is unavailable.")
      return
    }

    pendingPromise = promise

    val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
      addCategory(Intent.CATEGORY_OPENABLE)
      type = "text/*"
      putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("text/plain", "text/markdown", "text/csv", "application/json"))
      addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
      addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
    }

    try {
      activity.startActivityForResult(intent, REQUEST_CODE_PICK_TEXT)
    } catch (error: Exception) {
      pendingPromise = null
      promise.reject("E_FILE_IMPORT", error.message, error)
    }
  }

  private fun readImportedFile(uri: Uri): com.facebook.react.bridge.WritableMap {
    val resolver = reactApplicationContext.contentResolver
    val name = queryDisplayName(uri) ?: uri.lastPathSegment ?: "payload.txt"
    val size = querySize(uri)
    val mimeType = resolver.getType(uri)
    val text =
        resolver.openInputStream(uri)?.use { stream ->
          BufferedReader(InputStreamReader(stream, StandardCharsets.UTF_8)).use { reader ->
            reader.readText()
          }
        } ?: ""

    return Arguments.createMap().apply {
      putString("name", name)
      putDouble("size", size.toDouble())
      putString("mimeType", mimeType)
      putString("text", text)
    }
  }

  private fun queryDisplayName(uri: Uri): String? {
    val projection = arrayOf(OpenableColumns.DISPLAY_NAME)
    val cursor: Cursor? =
        reactApplicationContext.contentResolver.query(uri, projection, null, null, null)
    cursor?.use {
      val index = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
      if (index >= 0 && it.moveToFirst()) {
        return it.getString(index)
      }
    }
    return null
  }

  private fun querySize(uri: Uri): Long {
    val projection = arrayOf(OpenableColumns.SIZE)
    val cursor: Cursor? =
        reactApplicationContext.contentResolver.query(uri, projection, null, null, null)
    cursor?.use {
      val index = it.getColumnIndex(OpenableColumns.SIZE)
      if (index >= 0 && it.moveToFirst() && !it.isNull(index)) {
        return it.getLong(index)
      }
    }
    return 0L
  }

  companion object {
    private const val REQUEST_CODE_PICK_TEXT = 0x5346
  }
}
