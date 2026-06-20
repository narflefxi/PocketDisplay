package com.pocketdisplay.app

import android.os.Bundle
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class LicensesActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_FULLSCREEN
            or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_licenses)

        val text = resources.openRawResource(R.raw.third_party_licenses)
            .bufferedReader(Charsets.UTF_8)
            .use { it.readText() }
        findViewById<TextView>(R.id.tvLicenses).text = text
        findViewById<Button>(R.id.btnBack).setOnClickListener { finish() }
    }
}
