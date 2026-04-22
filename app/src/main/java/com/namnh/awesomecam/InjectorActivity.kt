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

        statusText.text = "Idle\n\nRequired assets: $HELPER_ASSET, $AGENT_ASSET, $SHADOWHOOK_LIB_NAME\nInject target: $RUNTIME_DIR/$SHADOWHOOK_LIB_NAME"
        logText.text = LOGCAT_PLACEHOLDER

        val injectButton: Button = findViewById(R.id.injectButton)
        val resetButton: Button = findViewById(R.id.resetButton)

        injectButton.setOnClickListener {
            runAsync {
                appendStatus("Preparing runtime files")
                val localHelper = extractAsset(HELPER_ASSET)
                val localAgent = extractAsset(AGENT_ASSET)
                val localShadowHook = extractBundledNativeLib(SHADOWHOOK_LIB_NAME)

                appendStatus(
                    "App-private staging ready:\n" +
                        (listOf(localHelper.absolutePath, localAgent.absolutePath) +
                            listOf(localShadowHook.absolutePath))
                            .joinToString("\n")
                )
                val commands = buildRuntimeCommands(localHelper, localAgent, localShadowHook)

                appendStatus("Running injector as root")
                appendStatus(runRoot(commands))
            }
        }

        resetButton.setOnClickListener {
            runAsync {
                appendStatus("Restarting cameraserver")
                appendStatus(runRoot(listOf("pkill cameraserver || killall cameraserver || true")))
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

    private fun buildRuntimeCommands(helper: File, agent: File, shadowHook: File): List<String> {
        val helperDst = "$RUNTIME_DIR/$HELPER_ASSET"
        val agentDst = "$RUNTIME_DIR/$AGENT_ASSET"
        val shadowHookDst = "$RUNTIME_DIR/$SHADOWHOOK_LIB_NAME"

        return buildList {
            add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
            add("cp ${shellQuote(helper.absolutePath)} ${shellQuote(helperDst)}")
            add("cp ${shellQuote(agent.absolutePath)} ${shellQuote(agentDst)}")
            add("cp ${shellQuote(shadowHook.absolutePath)} ${shellQuote(shadowHookDst)}")
            add("chmod 0755 ${shellQuote(helperDst)}")
            add("chmod 0644 ${shellQuote(agentDst)} ${shellQuote(shadowHookDst)}")
            add("chcon u:object_r:system_lib_file:s0 ${shellQuote(shadowHookDst)}")
            add("sh -c \"$helperDst cameraserver $shadowHookDst\"")
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
        private const val AGENT_ASSET = "agent.js"
        private const val SHADOWHOOK_LIB_NAME = "libshadowhook.so"
    }
}
