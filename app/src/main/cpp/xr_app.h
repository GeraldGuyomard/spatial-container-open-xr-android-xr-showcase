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

#include <atomic>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <memory>
#include <vector>

#include "renderer.h"

// Owns the OpenXR instance/session and a single self-rendering Spatial Container.
class XrApp {
public:
    XrApp(JavaVM* vm, jobject activity, Renderer::Backend backend)
        : vm_(vm), activity_(activity), backend_(backend) {}
    ~XrApp();

    bool Initialize();
    // Processes OpenXR events. Sets *exitRequested when the app should quit.
    void PollEvents(bool* exitRequested);
    bool IsSessionRunning() const { return sessionRunning_; }
    // Thread-safe: called from the UI thread.
    void RequestToggle() { toggleRequested_ = true; }
    bool IsImmersive() const { return immersive_; }
    void RenderFrame();
    const char* RendererName() const { return renderer_ ? renderer_->Name() : "none"; }

private:
    bool CreateInstance();
    bool InitializeSystem();
    bool CreateSession();
    void CreateActions();
    bool CreateContainer();
    void PollActions(XrTime now);
    XrVector3f ToggleCubeCenter() const;
    bool AimHitsToggleCube(const XrPosef& aim) const;

    // Half extent of the white toggle cube, in meters.
    static constexpr float kToggleCubeHalf = 0.04f;
    void RequestBoundsMode(XrSpatialContainerBoundsModeEXT mode);
    void UpdateRenderingState();

    JavaVM* vm_;
    jobject activity_;  // Global ref owned by the caller.
    Renderer::Backend backend_;
    std::atomic<bool> toggleRequested_{false};
    std::atomic<bool> immersive_{false};
    JNIEnv* env_ = nullptr;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrViewConfigurationType viewConfigType_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews_;
    bool hasHandInteraction_ = false;

    XrSpatialContainerEXT container_ = XR_NULL_HANDLE;
    XrSpace containerSpace_ = XR_NULL_HANDLE;
    bool containerVisible_ = false;
    bool containerInteractable_ = false;
    bool containerRendering_ = false;
    bool containerClosed_ = false;
    bool lastCanSubmit_ = false;
    XrSpatialContainerBoundsModeEXT boundsMode_ = XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT;
    XrExtent3Df bounds_ = {0.6f, 0.6f, 0.6f};
    bool infiniteBounds_ = false;
    // Last values logged for BOUNDS_CHANGED, to skip repeated identical events.
    XrExtent3Df lastLoggedBounds_ = {0, 0, 0};
    XrSpatialContainerBoundsModeEXT lastLoggedBoundsMode_ = XR_SPATIAL_CONTAINER_BOUNDS_MODE_MAX_ENUM_EXT;
    bool lastLoggedInfinite_ = false;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction selectAction_ = XR_NULL_HANDLE;
    XrAction aimAction_ = XR_NULL_HANDLE;
    XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace aimSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool toggleCubeHovered_ = false;

    std::unique_ptr<Renderer> renderer_;

    PFN_xrCreateSpatialContainerEXT xrCreateSpatialContainerEXT_ = nullptr;
    PFN_xrDestroySpatialContainerEXT xrDestroySpatialContainerEXT_ = nullptr;
    PFN_xrCreateSpatialContainerSpaceEXT xrCreateSpatialContainerSpaceEXT_ = nullptr;
    PFN_xrRequestSpatialContainerVisibleEXT xrRequestSpatialContainerVisibleEXT_ = nullptr;
    PFN_xrRequestSpatialContainerBoundsModeEXT xrRequestSpatialContainerBoundsModeEXT_ = nullptr;
    PFN_xrGetSpatialContainerBoundsEXT xrGetSpatialContainerBoundsEXT_ = nullptr;
    PFN_xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT
            xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT_ = nullptr;
    PFN_xrBeginSpatialContainerRenderingEXT xrBeginSpatialContainerRenderingEXT_ = nullptr;
    PFN_xrEndSpatialContainerRenderingEXT xrEndSpatialContainerRenderingEXT_ = nullptr;
    PFN_xrLocateSpatialContainerViewsEXT xrLocateSpatialContainerViewsEXT_ = nullptr;
};
