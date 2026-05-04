package com.companion_app.spectre

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import com.facebook.react.bridge.ActivityEventListener
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.BaseActivityEventListener
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableMap
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets

class SpectreFileImportModule(
  reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext) {

  companion object {
    private const val PICK_TEXT_FILE_REQUEST = 0x51B4
    private const val MAX_IMPORT_BYTES = 64 * 1024
  }

  private var pendingPromise: Promise? = null

  private val activityEventListener: ActivityEventListener =
    object : BaseActivityEventListener() {
      override fun onActivityResult(
        activity: Activity,
        requestCode: Int,
        resultCode: Int,
        data: Intent?,
      ) {
        if (requestCode != PICK_TEXT_FILE_REQUEST) {
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
          promise.resolve(readImport(uri))
        } catch (error: Exception) {
          promise.reject("spectre_file_import_read", error.message, error)
        }
      }
    }

  init {
    reactContext.addActivityEventListener(activityEventListener)
  }

  override fun getName(): String = "SpectreFileImport"

  @ReactMethod
  fun pickTextFile(promise: Promise) {
    if (pendingPromise != null) {
      promise.reject("spectre_file_import_busy", "A file picker request is already active.")
      return
    }

    val activity = reactApplicationContext.currentActivity
    if (activity == null) {
      promise.reject("spectre_file_import_activity", "No active Android activity.")
      return
    }

    val intent =
      Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
        addCategory(Intent.CATEGORY_OPENABLE)
        type = "*/*"
        putExtra(
          Intent.EXTRA_MIME_TYPES,
          arrayOf(
            "text/*",
            "application/octet-stream",
            "application/json",
          ),
        )
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
      }

    pendingPromise = promise
    try {
      activity.startActivityForResult(intent, PICK_TEXT_FILE_REQUEST)
    } catch (error: Exception) {
      pendingPromise = null
      promise.reject("spectre_file_import_launch", error.message, error)
    }
  }

  private fun readImport(uri: Uri): WritableMap {
    val resolver = reactApplicationContext.contentResolver
    val output = ByteArrayOutputStream()

    resolver.openInputStream(uri)?.use { stream ->
      val buffer = ByteArray(4096)
      while (true) {
        val read = stream.read(buffer)
        if (read <= 0) {
          break
        }

        output.write(buffer, 0, read)
        if (output.size() > MAX_IMPORT_BYTES) {
          throw IllegalStateException(
            "Selected file is too large for the current phone import path.",
          )
        }
      }
    } ?: throw IllegalStateException("Unable to open the selected file.")

    val bytes = output.toByteArray()
    val fileMeta = queryFileMeta(uri)

    return Arguments.createMap().apply {
      putString("name", fileMeta["name"] ?: "payload.txt")
      putDouble("size", bytes.size.toDouble())
      putString("mimeType", resolver.getType(uri))
      putString("text", String(bytes, StandardCharsets.UTF_8))
    }
  }

  private fun queryFileMeta(uri: Uri): Map<String, String?> {
    val resolver = reactApplicationContext.contentResolver
    resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
      if (cursor.moveToFirst()) {
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        val name =
          if (nameIndex >= 0) {
            cursor.getString(nameIndex)
          } else {
            null
          }
        return mapOf("name" to name)
      }
    }

    return mapOf("name" to uri.lastPathSegment)
  }
}
