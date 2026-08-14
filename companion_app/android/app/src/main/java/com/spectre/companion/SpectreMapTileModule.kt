package com.spectre.companion

import android.net.Uri
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Fetches OSM tiles with an application-specific User-Agent and keeps a
 * persistent seven-day cache. React Native's Android image loader does not
 * reliably forward per-image request headers, which causes the public OSM
 * service to return its tile-policy block image.
 */
class SpectreMapTileModule(
    reactContext: ReactApplicationContext,
) : ReactContextBaseJavaModule(reactContext) {
  private val executor = Executors.newFixedThreadPool(4)

  override fun getName(): String = "SpectreMapTiles"

  @ReactMethod
  fun getTile(zoom: Int, x: Int, y: Int, promise: Promise) {
    if (zoom !in 0..19) {
      promise.reject("INVALID_TILE", "Unsupported zoom $zoom")
      return
    }
    val tileCount = 1 shl zoom
    if (x !in 0 until tileCount || y !in 0 until tileCount) {
      promise.reject("INVALID_TILE", "Tile is outside the map")
      return
    }

    executor.execute {
      val tile = File(reactApplicationContext.filesDir, "map_tiles/$zoom/$x/$y.png")
      val sevenDaysMs = 7L * 24L * 60L * 60L * 1000L
      if (tile.isFile && System.currentTimeMillis() - tile.lastModified() < sevenDaysMs) {
        promise.resolve(Uri.fromFile(tile).toString())
        return@execute
      }

      try {
        tile.parentFile?.mkdirs()
        val connection = (URL("https://tile.openstreetmap.org/$zoom/$x/$y.png")
            .openConnection() as HttpURLConnection).apply {
          connectTimeout = 10_000
          readTimeout = 15_000
          instanceFollowRedirects = true
          setRequestProperty(
              "User-Agent",
              "SpectreField/0.1 (Aetherguard field locator; Android)",
          )
          setRequestProperty("Accept", "image/png,image/*;q=0.8")
        }
        try {
          if (connection.responseCode !in 200..299 ||
              !connection.contentType.orEmpty().startsWith("image/")) {
            throw IllegalStateException(
                "Map service returned ${connection.responseCode} ${connection.contentType}",
            )
          }
          val temporary = File(tile.parentFile, "${tile.name}.download")
          connection.inputStream.use { input ->
            temporary.outputStream().use { output -> input.copyTo(output) }
          }
          if (tile.exists()) tile.delete()
          if (!temporary.renameTo(tile)) {
            temporary.copyTo(tile, overwrite = true)
            temporary.delete()
          }
          promise.resolve(Uri.fromFile(tile).toString())
        } finally {
          connection.disconnect()
        }
      } catch (error: Exception) {
        // A stale map is more useful in the field than a blank map when the
        // network is unavailable.
        if (tile.isFile) {
          promise.resolve(Uri.fromFile(tile).toString())
        } else {
          promise.reject("TILE_FETCH_FAILED", error.message, error)
        }
      }
    }
  }
}
