package com.namnh.awesomecam

import android.content.Intent
import android.os.Bundle
import android.widget.ImageButton
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class LogsActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private lateinit var statusScroll: ScrollView
    private lateinit var logText: TextView
    private lateinit var logScroll: ScrollView

    private val listener = object : TelemetryStore.Listener {
        override fun onTelemetryChanged(snapshot: TelemetryStore.Snapshot) {
            runOnUiThread {
                statusText.text = snapshot.status.ifBlank { getString(R.string.status_empty) }
                logText.text = snapshot.log.ifBlank { getString(R.string.log_empty) }
                statusScroll.post { statusScroll.fullScroll(ScrollView.FOCUS_DOWN) }
                logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_logs)

        statusText = findViewById(R.id.statusText)
        statusScroll = findViewById(R.id.statusScroll)
        logText = findViewById(R.id.logText)
        logScroll = findViewById(R.id.logScroll)

        findViewById<ImageButton>(R.id.backButton).setOnClickListener {
            finish()
        }
        findViewById<TextView>(R.id.clearStatusButton).setOnClickListener {
            startActivity(Intent(this, InjectorActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
            finish()
        }
    }

    override fun onStart() {
        super.onStart()
        TelemetryStore.addListener(listener)
        TelemetryStore.acquireLogcat()
    }

    override fun onStop() {
        TelemetryStore.removeListener(listener)
        TelemetryStore.releaseLogcat()
        super.onStop()
    }
}
