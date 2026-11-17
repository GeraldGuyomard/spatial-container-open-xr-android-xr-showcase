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

#include <jni.h>
#include <openxr/openxr.h>

#include <memory>
#include <string>
#include <vector>

#include "scene.h"

// A graphics backend for the spatial container. XrApp owns the OpenXR instance, session and
// container; the backend owns the GPU device, the graphics binding and the swapchains.
//
// Call order: SelectInstanceExtensions -> (xrCreateInstance) -> Initialize -> SessionBinding ->
// (xrCreateSession) -> per frame: EnsureSwapchains + Render -> DestroySwapchains (before the
// session is destroyed) -> destructor (after the session is destroyed).
class Renderer {
public:
    enum class Backend { kWebGpu = 0, kVulkan = 1 };

    virtual ~Renderer() = default;

    virtual const char* Name() const = 0;

    // Appends the OpenXR instance extensions this backend needs. Returns false if a required one
    // is missing from `available`.
    virtual bool SelectInstanceExtensions(const std::vector<std::string>& available,
                                          std::vector<const char*>* enabled) = 0;

    // Creates the GPU device for this OpenXR system.
    virtual bool Initialize(XrInstance instance, XrSystemId systemId) = 0;

    // Graphics binding structure to chain into XrSessionCreateInfo (valid after Initialize).
    virtual const void* SessionBinding() const = 0;

    // (Re)creates the swapchains for both eyes side by side, eyeWidth x eyeHeight each.
    virtual bool EnsureSwapchains(XrSession session, uint32_t eyeWidth, uint32_t eyeHeight) = 0;
    virtual void DestroySwapchains() = 0;

    // Renders the scene and fills each view's subImage (and, if supported, chains layer depth info
    // owned by the renderer). Returns false if nothing should be submitted this frame.
    virtual bool Render(const EyeView (&eyes)[2], const SceneState& scene,
                        XrCompositionLayerProjectionView (&views)[2]) = 0;
};

std::unique_ptr<Renderer> CreateWebGpuRenderer(JavaVM* vm);  // webgpu_renderer.cpp
std::unique_ptr<Renderer> CreateVulkanRenderer();            // vulkan_renderer.cpp

inline std::unique_ptr<Renderer> CreateRenderer(Renderer::Backend backend, JavaVM* vm) {
    return backend == Renderer::Backend::kVulkan ? CreateVulkanRenderer()
                                                 : CreateWebGpuRenderer(vm);
}
