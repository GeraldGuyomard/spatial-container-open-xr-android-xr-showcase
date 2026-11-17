/*
 * Copyright 2026 Gerald Guyomard
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.geraldguyomard.spatialcontainer

import android.app.Activity
import android.util.Log

/** Log tag shared with the native code (log.h): `adb logcat -s SCDemo`. */
const val LogTag = "SCDemo"

/**
 * Native OpenXR spatial container, rendered with WebGPU (Dawn) or directly with Vulkan. It runs its
 * own frame loop on a native thread, next to the activity's Compose XR UI.
 */
object ContainerRenderer {
    /** Rendering backend; values match Renderer::Backend in renderer.h. */
    enum class Backend(val label: String) {
        WEBGPU("WebGPU"),
        VULKAN("Vulkan"),
    }

    init {
        System.loadLibrary("spatialcontainer")
    }

    /** Starts the container's OpenXR session and frame loop. */
    fun start(activity: Activity) {
        Log.i(LogTag, "Starting container renderer from ${activity.javaClass.simpleName}")
        nativeStart(activity)
    }

    /** Stops the frame loop and tears down the OpenXR session. */
    fun stop() {
        Log.i(LogTag, "Stopping container renderer")
        nativeStop()
    }

    /** Requests a switch between bounded and immersive modes. */
    fun toggleBoundsMode() {
        Log.i(LogTag, "Toggle button pressed")
        nativeToggleBoundsMode()
    }

    /**
     * Switches the rendering backend. This recreates the OpenXR session and the container, and
     * blocks until the old one is torn down: call it off the main thread.
     */
    fun setBackend(backend: Backend) {
        Log.i(LogTag, "Switching renderer to ${backend.label}")
        nativeSetBackend(backend.ordinal)
    }

    fun backend(): Backend = Backend.entries[nativeGetBackend()]

    /** Whether the runtime has confirmed immersive mode for the container. */
    fun isImmersive(): Boolean = nativeIsImmersive()

    private external fun nativeStart(activity: Activity)
    private external fun nativeStop()
    private external fun nativeToggleBoundsMode()
    private external fun nativeIsImmersive(): Boolean
    private external fun nativeSetBackend(backend: Int)
    private external fun nativeGetBackend(): Int
}
