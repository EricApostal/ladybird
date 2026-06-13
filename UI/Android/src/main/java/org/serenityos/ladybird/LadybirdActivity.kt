/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
package org.serenityos.ladybird

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.nio.file.Files
import java.util.zip.ZipFile
import kotlin.io.path.Path
import kotlin.io.path.inputStream
import kotlin.io.path.isDirectory
import kotlin.io.path.outputStream
import org.serenityos.ladybird.databinding.ActivityMainBinding

class LadybirdActivity : AppCompatActivity() {

    private val logTag = "LadybirdActivity"
    private val eventLoopPumpIntervalMs = 16L
    private val eventLoopHandler = Handler(Looper.getMainLooper())
    private var eventLoopPumpCount = 0L
    private var callbackPumpCount = 0L

    private lateinit var binding: ActivityMainBinding
    private lateinit var resourceDir: String
    private lateinit var view: WebView
    private lateinit var urlEditText: EditText
    private var timerService = TimerExecutorService()

    private val eventLoopPumpRunnable =
            object : Runnable {
                override fun run() {
                    execMainEventLoop()
                    eventLoopPumpCount += 1
                    if (eventLoopPumpCount <= 10L || eventLoopPumpCount % 120L == 0L)
                            Log.i(logTag, "Periodic event loop pump #$eventLoopPumpCount")
                    eventLoopHandler.postDelayed(this, eventLoopPumpIntervalMs)
                }
            }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        resourceDir = TransferAssets.transferAssets(this)
        val testFile = File("$resourceDir/icons/48x48/app-browser.png")
        if (!testFile.exists()) {
            ZipFile("$resourceDir/ladybird-assets.zip").use { zip ->
                zip.entries().asSequence().forEach { entry ->
                    val fileName = entry.name
                    val file = File("$resourceDir/$fileName")
                    if (!entry.isDirectory) {
                        val parentFolder = File(file.parent!!)
                        if (!parentFolder.exists()) parentFolder.mkdirs()
                        zip.getInputStream(entry).use { input ->
                            file.outputStream().use { output -> input.copyTo(output) }
                        }
                    }
                }
            }

            // curl has some issues with Android's way of storing certificates.
            val certMain = File("$resourceDir/cacert.pem")
            certMain.outputStream().use { output ->
                Files.walk(Path("/system/etc/security/cacerts")).forEach { certPath ->
                    if (!certPath.isDirectory()) {
                        certPath.inputStream().use { input -> input.copyTo(output) }
                    }
                }
            }
        }

        val userDir = applicationContext.getExternalFilesDir(null)!!.absolutePath
        val nativeLibraryDir = applicationInfo.nativeLibraryDir
        initNativeCode(resourceDir, "Ladybird", timerService, userDir, nativeLibraryDir)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)

        urlEditText = binding.urlEditText
        view = binding.webView

        view.onLoadStart = { url: String, _ ->
            Log.i(logTag, "onLoadStart callback: $url")
            urlEditText.setText(url, TextView.BufferType.EDITABLE)
        }

        urlEditText.setOnEditorActionListener { textView: TextView, actionId: Int, _: KeyEvent? ->
            when (actionId) {
                EditorInfo.IME_ACTION_GO, EditorInfo.IME_ACTION_SEARCH -> {
                    val requestedUrl = normalizeInputUrl(textView.text.toString())
                    Log.i(logTag, "URL bar load request: $requestedUrl")
                    view.loadURL(requestedUrl)
                    urlEditText.setText(requestedUrl, TextView.BufferType.EDITABLE)
                    true
                }
                else -> false
            }
        }

        view.initialize()
        val initialUrl = normalizeInputUrl(intent.dataString ?: "https://ladybird.org")
        Log.i(logTag, "Initial URL request: $initialUrl")
        urlEditText.setText(initialUrl, TextView.BufferType.EDITABLE)
        view.post {
            Log.i(
                    logTag,
                    "Dispatching initial URL after layout: ${view.width}x${view.height} -> $initialUrl"
            )
            view.loadURL(initialUrl)
        }

        Log.i(logTag, "Starting periodic event loop pump every ${eventLoopPumpIntervalMs}ms")
        eventLoopHandler.post(eventLoopPumpRunnable)
    }

    override fun onStart() {
        super.onStart()
    }

    override fun onDestroy() {
        eventLoopHandler.removeCallbacks(eventLoopPumpRunnable)
        view.dispose()
        disposeNativeCode()
        super.onDestroy()
    }

    private fun scheduleEventLoop() {
        mainExecutor.execute {
            execMainEventLoop()
            callbackPumpCount += 1
            if (callbackPumpCount <= 10L || callbackPumpCount % 120L == 0L)
                    Log.i(logTag, "Callback-driven event loop pump #$callbackPumpCount")
        }
    }

    private fun normalizeInputUrl(value: String): String {
        val trimmed = value.trim()
        if (trimmed.isEmpty()) return "about:blank"
        if (trimmed.contains("://")) return trimmed
        return "https://$trimmed"
    }

    private external fun initNativeCode(
            resourceDir: String,
            tag: String,
            timerService: TimerExecutorService,
            userDir: String,
            nativeLibraryDir: String
    )

    private external fun disposeNativeCode()
    private external fun execMainEventLoop()

    companion object {
        // Used to load the 'Ladybird' library on application startup.
        init {
            System.loadLibrary("Ladybird")
        }
    }
}
