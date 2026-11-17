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

// Spatial Container demo: a self-rendering OpenXR spatial container whose content is drawn with
// WebGPU (Dawn) or Vulkan, driven from a Kotlin activity that also shows a Compose XR SpatialPanel. The
// OpenXR frame loop runs on its own thread. Aim at the white cube and pinch, or press the panel's
// button, to toggle between bounded and immersive modes.

#include <jni.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "log.h"
#include "xr_app.h"

namespace {

JavaVM* gVm = nullptr;
jobject gActivity = nullptr;
std::thread gThread;
std::atomic<bool> gStop{false};
std::atomic<int> gBackend{static_cast<int>(Renderer::Backend::kWebGpu)};
std::mutex gLifecycleMutex;  // Serializes start/stop/switch (switch runs off the UI thread).
std::mutex gAppMutex;
XrApp* gApp = nullptr;  // Guarded by gAppMutex; owned by the render thread.

void RenderLoop(Renderer::Backend backend) {
    XrApp xrApp(gVm, gActivity, backend);
    if (!xrApp.Initialize()) {
        LOGE("Initialization failed");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gAppMutex);
        gApp = &xrApp;
    }
    bool exitRequested = false;
    while (!gStop && !exitRequested) {
        xrApp.PollEvents(&exitRequested);
        if (xrApp.IsSessionRunning()) {
            xrApp.RenderFrame();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::lock_guard<std::mutex> lock(gAppMutex);
    gApp = nullptr;
    // xrApp is destroyed here, on the thread that created it.
}

void StartLoop() {
    gStop = false;
    gThread = std::thread(RenderLoop, static_cast<Renderer::Backend>(gBackend.load()));
}

void StopLoop() {
    gStop = true;
    if (gThread.joinable()) gThread.join();
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_geraldguyomard_spatialcontainer_ContainerRenderer_nativeStart(JNIEnv* env, jobject, jobject activity) {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    if (gThread.joinable()) return;
    env->GetJavaVM(&gVm);
    gActivity = env->NewGlobalRef(activity);
    StartLoop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_geraldguyomard_spatialcontainer_ContainerRenderer_nativeStop(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    StopLoop();
    if (gActivity != nullptr) {
        env->DeleteGlobalRef(gActivity);
        gActivity = nullptr;
    }
}

// The graphics binding is fixed when the OpenXR session is created, so switching backend tears down
// the whole OpenXR app (session and container) and starts a new one with the other renderer.
// Blocks until the old session is torn down: call it off the UI thread.
extern "C" JNIEXPORT void JNICALL
Java_com_geraldguyomard_spatialcontainer_ContainerRenderer_nativeSetBackend(JNIEnv*, jobject, jint backend) {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    if (backend == gBackend) return;
    gBackend = backend;
    if (gActivity == nullptr) return;  // Not started yet: used on the next start.
    LOGI("Switching renderer: restarting the OpenXR session");
    StopLoop();
    StartLoop();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_geraldguyomard_spatialcontainer_ContainerRenderer_nativeGetBackend(JNIEnv*, jobject) {
    return gBackend;
}

extern "C" JNIEXPORT void JNICALL
Java_com_geraldguyomard_spatialcontainer_ContainerRenderer_nativeToggleBoundsMode(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(gAppMutex);
    if (gApp != nullptr) gApp->RequestToggle();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_geraldguyomard_spatialcontainer_ContainerRenderer_nativeIsImmersive(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(gAppMutex);
    return gApp != nullptr && gApp->IsImmersive();
}
