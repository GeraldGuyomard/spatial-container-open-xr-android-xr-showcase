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

#pragma once

#include <android/log.h>

// Every log line of the demo, native and Kotlin, uses this tag (see LogTag in ContainerRenderer.kt),
// so `adb logcat -s SCDemo` shows only the demo's own messages.
#define SCDEMO_TAG "SCDemo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, SCDEMO_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, SCDEMO_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SCDEMO_TAG, __VA_ARGS__)
