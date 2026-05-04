package com.namnh.awesomecam

import java.io.File
import kotlin.concurrent.thread

class Mp4FeedController(
    private val postStatus: (String) -> Unit,
) {
    @Volatile
    private var running: Boolean = false

    @Volatile
    private var worker: Thread? = null

    val isRunning: Boolean
        get() = running

    fun start(path: String = DEFAULT_VIDEO_PATH) {
        if (running) return
        worker = thread(start = true, name = "mp4-feed-control") {
            try {
                val source = prepareReadableSource(path)
                if (!VideoBridge.connect()) {
                    postStatus("Video feed: binder service unavailable. Inject hook first.")
                    return@thread
                }
                postStatus("Video feed: starting native playback $source")
                if (!VideoBridge.playFile(source)) {
                    postStatus("Video feed: native playback start failed for $source")
                    return@thread
                }
                running = true
                postStatus("Video feed: native playback active $source")
            } catch (t: Throwable) {
                postStatus("Video feed error: ${t.message}")
            } finally {
                worker = null
            }
        }
    }

    fun stop() {
        worker?.interrupt()
        worker = null
        if (!running) {
            VideoBridge.clear()
            return
        }
        running = false
        thread(start = true, name = "mp4-feed-stop") {
            val stopped = VideoBridge.stopPlayback()
            VideoBridge.clear()
            postStatus(
                if (stopped) "Video feed: native playback stopped" else
                    "Video feed: stop requested (service may already be idle)",
            )
        }
    }

    private fun prepareReadableSource(path: String): String {
        val source = File(path)
        require(source.exists()) { "Missing source $path" }
        return source.absolutePath
    }

    companion object {
        const val DEFAULT_VIDEO_PATH: String = "/data/camera/input.mp4"
    }
}
