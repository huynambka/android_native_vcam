package com.namnh.awesomecam

import java.io.File
import kotlin.concurrent.thread

class Mp4FeedController(
    private val postStatus: (String) -> Unit,
) {
    @Volatile
    private var worker: Thread? = null

    @Volatile
    private var active: Boolean = false

    val isRunning: Boolean
        get() = active

    fun start(path: String = DEFAULT_VIDEO_PATH) {
        if (isRunning) return
        worker = thread(start = true, name = "ffmpeg-player-control") {
            try {
                val source = prepareReadableSource(path)
                postStatus("Video feed: starting native FFmpeg player for $source")
                val output = runRoot(buildStartCommands(source))
                active = output.lineSequence().any { it.trim().toIntOrNull() != null }
                postStatus("Video feed: native player start requested\n$output")
                if (active) {
                    postStatus("Video feed: open camera/webcam preview and watch FFmpegPlayer + source=FFmpegPlayback logs")
                } else {
                    postStatus("Video feed: player PID was not observed; check /data/camera/awesomecam_player.log")
                }
            } catch (t: Throwable) {
                active = false
                postStatus("Video feed error: ${t.message}")
            } finally {
                worker = null
            }
        }
    }

    fun stop() {
        worker?.interrupt()
        worker = null
        thread(start = true, name = "ffmpeg-player-stop") {
            val output = runRoot(buildStopCommands())
            runCatching { VideoBridge.clear() }
            active = false
            postStatus("Video feed: native player stop requested; source cache cleared\n$output")
        }
    }

    private fun prepareReadableSource(path: String): String {
        val source = File(path)
        require(source.exists()) { "Missing source $path" }
        return source.absolutePath
    }

    private fun buildStartCommands(source: String): List<String> = listOf(
        "mkdir -p ${shellQuote(RUNTIME_DIR)}",
        "rm -f ${shellQuote(PLAYER_PID)}",
        "export LD_LIBRARY_PATH=${shellQuote(RUNTIME_DIR)}:\${LD_LIBRARY_PATH}",
        "cd ${shellQuote(RUNTIME_DIR)}",
        "${shellQuote(PLAYER_PATH)} --input ${shellQuote(source)} --loop --pidfile ${shellQuote(PLAYER_PID)} >> ${shellQuote(PLAYER_LOG)} 2>&1 &",
        "sleep 0.2",
        "cat ${shellQuote(PLAYER_PID)} 2>/dev/null || true",
    )

    private fun buildStopCommands(): List<String> = listOf(
        "if [ -f ${shellQuote(PLAYER_PID)} ]; then kill -TERM $(cat ${shellQuote(PLAYER_PID)}) 2>/dev/null || true; fi",
        "pkill -TERM -f ${shellQuote(PLAYER_PATH)} 2>/dev/null || true",
        "rm -f ${shellQuote(PLAYER_PID)}",
    )

    private fun runRoot(commands: List<String>): String {
        val process = ProcessBuilder("su")
            .redirectErrorStream(true)
            .start()
        process.outputStream.bufferedWriter().use { writer ->
            for (command in commands) {
                writer.write(command)
                writer.newLine()
            }
            writer.write("exit")
            writer.newLine()
            writer.flush()
        }
        val output = process.inputStream.bufferedReader().readText()
        val exitCode = process.waitFor()
        return "exit=$exitCode\n" + output.ifBlank { "(no output)" }
    }

    private fun shellQuote(value: String): String {
        return "'" + value.replace("'", "'\"'\"'") + "'"
    }

    companion object {
        const val DEFAULT_VIDEO_PATH: String = "/data/camera/input.mp4"
        private const val RUNTIME_DIR = "/data/camera"
        private const val PLAYER_PATH = "$RUNTIME_DIR/awesomecam_player"
        private const val PLAYER_PID = "$RUNTIME_DIR/awesomecam_player.pid"
        private const val PLAYER_LOG = "$RUNTIME_DIR/awesomecam_player.log"
    }
}
