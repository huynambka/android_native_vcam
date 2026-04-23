package com.namnh.awesomecam

import android.os.Bundle
import android.os.Build
import android.widget.Button
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.util.zip.ZipFile
import kotlin.concurrent.thread

class InjectorActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private lateinit var statusScroll: ScrollView
    private lateinit var logText: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var feedController: Mp4FeedController

    @Volatile
    private var logcatProcess: Process? = null

    @Volatile
    private var logcatThread: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_injector)

        statusText = findViewById(R.id.statusText)
        statusScroll = findViewById(R.id.statusScroll)
        logText = findViewById(R.id.logText)
        logScroll = findViewById(R.id.logScroll)
        feedController = Mp4FeedController(::appendStatus)

        statusText.text =
            "Idle\n\nRequired assets: $HELPER_ASSET, $SHADOWHOOK_LIB_NAME, $HOOK_ASSET\n" +
                "Stage 1: $RUNTIME_DIR/$SHADOWHOOK_LIB_NAME\n" +
                "Stage 2: $RUNTIME_DIR/$HOOK_ASSET\n\n" +
                "File fallback: $RUNTIME_DIR/source.meta or $RUNTIME_DIR/frames/*.i420|nv12|nv21\n" +
                "Binder-fed MP4: ${Mp4FeedController.DEFAULT_VIDEO_PATH}"
        logText.text = LOGCAT_PLACEHOLDER

        val injectButton: Button = findViewById(R.id.injectButton)
        val feedButton: Button = findViewById(R.id.feedButton)
        val resetButton: Button = findViewById(R.id.resetButton)

        injectButton.setOnClickListener {
            runAsync {
                appendStatus("Preparing runtime files")
                val localHelper = extractAsset(HELPER_ASSET)
                val localHook = extractAsset(HOOK_ASSET)
                val localShadowHook = extractBundledNativeLib(SHADOWHOOK_LIB_NAME)

                appendStatus(
                    "App-private staging ready:\n" +
                        (listOf(localHelper.absolutePath, localHook.absolutePath) +
                            listOf(localShadowHook.absolutePath))
                            .joinToString("\n")
                )
                val commands = buildRuntimeCommands(localHelper, localShadowHook, localHook)

                appendStatus("Running injector as root")
                appendStatus(runRoot(commands))
            }
        }

        feedButton.setOnClickListener {
            if (feedController.isRunning) {
                appendStatus("Stopping MP4 feed")
                feedController.stop()
                feedButton.text = getString(R.string.feed_button_start)
            } else {
                appendStatus("Starting MP4 feed from ${Mp4FeedController.DEFAULT_VIDEO_PATH}")
                feedController.start(Mp4FeedController.DEFAULT_VIDEO_PATH)
                feedButton.text = getString(R.string.feed_button_stop)
            }
        }

        resetButton.setOnClickListener {
            runAsync {
                appendStatus("Restarting cameraserver")
                feedController.stop()
                appendStatus(runRoot(listOf("pkill cameraserver || killall cameraserver || true")))
                runOnUiThread { feedButton.text = getString(R.string.feed_button_start) }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        startLogcat()
    }

    override fun onStop() {
        stopLogcat()
        super.onStop()
    }

    override fun onDestroy() {
        feedController.stop()
        super.onDestroy()
    }

    private fun runAsync(block: () -> Unit) {
        thread(start = true) {
            try {
                block()
            } catch (t: Throwable) {
                appendStatus("Error: ${t.message}")
            }
        }
    }

    private fun extractAsset(name: String): File {
        val outFile = File(filesDir, name)
        assets.open(name).use { input ->
            outFile.outputStream().use { output ->
                input.copyTo(output)
            }
        }
        return outFile
    }

    private fun extractBundledNativeLib(libName: String): File {
        val outFile = File(filesDir, libName)

        val abiCandidates = buildList {
            addAll(Build.SUPPORTED_ABIS.map { abi -> "lib/$abi/$libName" })
            add("lib/arm64-v8a/$libName")
            add("lib/armeabi-v7a/$libName")
            add("lib/x86_64/$libName")
            add("lib/x86/$libName")
        }.distinct()

        ZipFile(applicationInfo.sourceDir).use { zip ->
            val entry = abiCandidates
                .asSequence()
                .mapNotNull { path -> zip.getEntry(path) }
                .firstOrNull()
                ?: error("Missing bundled native lib in APK: $libName")

            zip.getInputStream(entry).use { input ->
                outFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        }

        return outFile
    }

    private fun buildRuntimeCommands(helper: File, shadowHook: File, hook: File): List<String> {
        val helperDst = "$RUNTIME_DIR/$HELPER_ASSET"
        val shadowHookDst = "$RUNTIME_DIR/$SHADOWHOOK_LIB_NAME"
        val hookDst = "$RUNTIME_DIR/$HOOK_ASSET"

        return buildList {
            add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
            add("cp ${shellQuote(helper.absolutePath)} ${shellQuote(helperDst)}")
            add("cp ${shellQuote(shadowHook.absolutePath)} ${shellQuote(shadowHookDst)}")
            add("cp ${shellQuote(hook.absolutePath)} ${shellQuote(hookDst)}")
            add("chmod 0755 ${shellQuote(helperDst)}")
            add("chmod 0644 ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            add("chcon u:object_r:system_lib_file:s0 ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            add("sh -c \"$helperDst cameraserver $shadowHookDst\"")
            add("sh -c \"$helperDst cameraserver $hookDst\"")
        }
    }

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
        return buildString {
            append("exit=")
            append(exitCode)
            append('\n')
            append(output.ifBlank { "(no output)" })
        }
    }

    private fun startLogcat() {
        if (logcatThread?.isAlive == true) {
            return
        }

        val readerThread = thread(start = false, name = "logcat-reader") {
            var process: Process? = null
            try {
                process = ProcessBuilder("su", "-c", LOGCAT_COMMAND)
                    .redirectErrorStream(true)
                    .start()
                logcatProcess = process

                process.inputStream.bufferedReader().use { reader ->
                    while (true) {
                        val line = reader.readLine() ?: break
                        appendLogLine(line)
                    }
                }

                val exitCode = process.waitFor()
                if (logcatProcess === process && exitCode != 0) {
                    appendLogLine("logcat exited with code $exitCode")
                }
            } catch (t: Throwable) {
                if (logcatProcess === process) {
                    appendLogLine("Logcat error: ${t.message}")
                }
            } finally {
                if (logcatProcess === process) {
                    logcatProcess = null
                }
                if (logcatThread === Thread.currentThread()) {
                    logcatThread = null
                }
            }
        }

        logcatThread = readerThread
        readerThread.start()
    }

    private fun stopLogcat() {
        val process = logcatProcess
        logcatProcess = null
        logcatThread?.interrupt()
        logcatThread = null
        process?.destroy()
    }

    private fun appendStatus(message: String) {
        runOnUiThread {
            statusText.text = trimOutput(
                buildString {
                    if (statusText.text.isNotBlank()) {
                        append(statusText.text)
                        append("\n\n")
                    }
                    append(message)
                }
            )
            statusScroll.post { statusScroll.fullScroll(ScrollView.FOCUS_DOWN) }
        }
    }

    private fun appendLogLine(line: String) {
        runOnUiThread {
            val current = logText.text.toString()
            logText.text = trimOutput(
                if (current.isBlank() || current == LOGCAT_PLACEHOLDER) {
                    line
                } else {
                    "$current\n$line"
                }
            )
            logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
        }
    }

    private fun trimOutput(text: String): String {
        return if (text.length <= MAX_OUTPUT_CHARS) {
            text
        } else {
            text.takeLast(MAX_OUTPUT_CHARS).trimStart()
        }
    }

    private fun shellQuote(value: String): String {
        return "'" + value.replace("'", "'\"'\"'") + "'"
    }

    companion object {
        private const val LOGCAT_PLACEHOLDER = "Waiting for logcat..."
        private const val LOGCAT_COMMAND = "logcat -v time -s awesomeCAM:I *:S"
        private const val MAX_OUTPUT_CHARS = 16000
        private const val RUNTIME_DIR = "/data/camera"
        private const val HELPER_ASSET = "injector_helper"
        private const val HOOK_ASSET = "libhook.so"
        private const val SHADOWHOOK_LIB_NAME = "libshadowhook.so"
    }
}
