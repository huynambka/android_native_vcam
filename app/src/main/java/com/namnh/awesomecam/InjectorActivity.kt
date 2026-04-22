package com.namnh.awesomecam

import android.os.Bundle
import android.widget.Button
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
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

        statusText.text = "Idle\n\nRequired assets: injector_helper, agent.js, libtest_inject.so"
        logText.text = LOGCAT_PLACEHOLDER

        val injectButton: Button = findViewById(R.id.injectButton)
        val resetButton: Button = findViewById(R.id.resetButton)

        injectButton.setOnClickListener {
            runAsync {
                appendStatus("Preparing runtime files")
                val localHelper = extractAsset(HELPER_ASSET)
                val localAgent = extractAsset(AGENT_ASSET)
                val localPayload = extractAsset(PAYLOAD_ASSET)

                appendStatus("App-private staging ready:\n${localHelper.absolutePath}\n${localAgent.absolutePath}\n${localPayload.absolutePath}")
                val commands = buildRuntimeCommands(localHelper, localAgent, localPayload)

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

    private fun buildRuntimeCommands(helper: File, agent: File, payload: File): List<String> {
        val helperDst = "$RUNTIME_DIR/$HELPER_ASSET"
        val agentDst = "$RUNTIME_DIR/$AGENT_ASSET"
        val payloadDst = "$RUNTIME_DIR/$PAYLOAD_ASSET"

        return listOf(
            "mkdir -p ${shellQuote(RUNTIME_DIR)}",
            "cp ${shellQuote(helper.absolutePath)} ${shellQuote(helperDst)}",
            "cp ${shellQuote(agent.absolutePath)} ${shellQuote(agentDst)}",
            "cp ${shellQuote(payload.absolutePath)} ${shellQuote(payloadDst)}",
            "chmod 0755 ${shellQuote(helperDst)}",
            "chmod 0644 ${shellQuote(agentDst)} ${shellQuote(payloadDst)}",
            "chcon u:object_r:system_lib_file:s0 ${shellQuote(payloadDst)}",
            "sh -c \"selinux=${'$'}(getenforce 2>/dev/null || echo Unknown); echo SELinux=${'$'}selinux; if [ \\\"${'$'}selinux\\\" = Enforcing ]; then setenforce 0; fi; \\\"$helperDst\\\" cameraserver \\\"$payloadDst\\\"; rc=${'$'}?; if [ \\\"${'$'}selinux\\\" = Enforcing ]; then setenforce 1; fi; exit ${'$'}rc\""
        )
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
        private const val RUNTIME_DIR = "/data/local/tmp/awesomeCAM"
        private const val HELPER_ASSET = "injector_helper"
        private const val AGENT_ASSET = "agent.js"
        private const val PAYLOAD_ASSET = "libtest_inject.so"
    }
}
