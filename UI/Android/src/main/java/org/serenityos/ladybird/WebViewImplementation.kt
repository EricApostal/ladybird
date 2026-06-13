/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
package org.serenityos.ladybird

import android.graphics.Bitmap
import android.util.Log

/** Wrapper around WebView::ViewImplementation for use by Kotlin */
class WebViewImplementation(private val view: WebView) {
    private val logTag = "LadybirdWebViewImpl"

    // Instance Pointer to native object, very unsafe :)
    private var nativeInstance: Long = 0
    private var invalidateCount: Long = 0
    private var loadStartCount: Long = 0

    fun initialize() {
        nativeInstance = nativeObjectInit()
        Log.i(logTag, "initialize nativeInstance=$nativeInstance")
    }

    fun dispose() {
        Log.i(logTag, "dispose nativeInstance=$nativeInstance")
        nativeObjectDispose(nativeInstance)
        nativeInstance = 0
    }

    fun loadURL(url: String) {
        Log.i(logTag, "loadURL request: $url")
        nativeLoadURL(nativeInstance, url)
    }

    fun drawIntoBitmap(bitmap: Bitmap) {
        nativeDrawIntoBitmap(nativeInstance, bitmap)
    }

    fun setViewportGeometry(w: Int, h: Int) {
        nativeSetViewportGeometry(nativeInstance, w, h)
    }

    fun setDevicePixelRatio(ratio: Float) {
        nativeSetDevicePixelRatio(nativeInstance, ratio)
    }

    fun mouseEvent(eventType: Int, x: Float, y: Float, rawX: Float, rawY: Float) {
        nativeMouseEvent(nativeInstance, eventType, x, y, rawX, rawY)
    }

    // Functions called from native code
    fun invalidateLayout() {
        invalidateCount += 1
        if (invalidateCount <= 10L || invalidateCount % 30L == 0L)
                Log.i(logTag, "invalidateLayout #$invalidateCount (frame-ready signal from native)")
        view.requestLayout()
        view.invalidate()
    }

    fun onLoadStart(url: String, isRedirect: Boolean) {
        loadStartCount += 1
        Log.i(logTag, "onLoadStart #$loadStartCount url=$url isRedirect=$isRedirect")
        view.onLoadStart(url, isRedirect)
    }

    // Functions implemented in native code
    private external fun nativeObjectInit(): Long
    private external fun nativeObjectDispose(instance: Long)

    private external fun nativeDrawIntoBitmap(instance: Long, bitmap: Bitmap)
    private external fun nativeSetViewportGeometry(instance: Long, w: Int, h: Int)
    private external fun nativeSetDevicePixelRatio(instance: Long, ratio: Float)
    private external fun nativeLoadURL(instance: Long, url: String)
    private external fun nativeMouseEvent(
            instance: Long,
            eventType: Int,
            x: Float,
            y: Float,
            rawX: Float,
            rawY: Float
    )

    companion object {
        /*
         * We use a static class initializer to allow the native code to cache some
         * field offsets. This native function looks up and caches interesting
         * class/field/method IDs. Throws on failure.
         */
        private external fun nativeClassInit()

        init {
            nativeClassInit()
        }
    }
}
