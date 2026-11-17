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

#include "xr_app.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>


#define XR_CHECK(call)                                                       \
    do {                                                                     \
        XrResult result_ = (call);                                           \
        if (XR_FAILED(result_)) {                                            \
            LOGE("%s failed: %d (%s:%d)", #call, result_, __FILE__, __LINE__); \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

constexpr XrExtent3Df kSuggestedBounds = {0.6f, 0.6f, 0.6f};

template <typename T>
bool LoadFunction(XrInstance instance, const char* name, T* function) {
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name,
                                        reinterpret_cast<PFN_xrVoidFunction*>(function)))) {
        LOGE("Missing OpenXR function %s", name);
        return false;
    }
    return true;
}

bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

}  // namespace

XrApp::~XrApp() {
    if (renderer_) renderer_->DestroySwapchains();
    for (XrSpace space : aimSpaces_) {
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
    }
    if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
    if (containerRendering_ && xrEndSpatialContainerRenderingEXT_) {
        XrSpatialContainerEndInfoEXT endInfo{XR_TYPE_SPATIAL_CONTAINER_END_INFO_EXT};
        endInfo.spatialContainer = container_;
        xrEndSpatialContainerRenderingEXT_(session_, &endInfo);
    }
    if (containerSpace_ != XR_NULL_HANDLE) xrDestroySpace(containerSpace_);
    if (container_ != XR_NULL_HANDLE) xrDestroySpatialContainerEXT_(container_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    renderer_.reset();  // GPU device after the session that used it.
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    if (env_ != nullptr) vm_->DetachCurrentThread();
}

bool XrApp::Initialize() {
    vm_->AttachCurrentThread(&env_, nullptr);

    renderer_ = CreateRenderer(backend_, vm_);
    LOGI("Renderer: %s", renderer_->Name());

    return CreateInstance() && InitializeSystem() && renderer_->Initialize(instance_, systemId_) &&
           CreateSession() && CreateContainer();
}

bool XrApp::CreateInstance() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (!LoadFunction(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &initializeLoader)) return false;
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = vm_;
    loaderInfo.applicationContext = activity_;
    XR_CHECK(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)));

    uint32_t count = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));

    std::vector<const char*> extensions = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_EXT_SPATIAL_CONTAINER_EXTENSION_NAME,
        XR_EXT_SPATIAL_CONTAINER_SELF_RENDERING_EXTENSION_NAME,
    };
    for (const char* name : extensions) {
        if (!HasExtension(available, name)) {
            LOGE("Required extension %s is not supported", name);
            return false;
        }
    }
    hasHandInteraction_ = HasExtension(available, XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    if (hasHandInteraction_) extensions.push_back(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);

    std::vector<std::string> availableNames;
    for (const auto& extension : available) availableNames.emplace_back(extension.extensionName);
    if (!renderer_->SelectInstanceExtensions(availableNames, &extensions)) return false;

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = vm_;
    androidInfo.applicationActivity = activity_;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "SpatialContainerDemo",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, renderer_->Name(), XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_1;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    XR_CHECK(xrCreateInstance(&createInfo, &instance_));

    return LoadFunction(instance_, "xrCreateSpatialContainerEXT", &xrCreateSpatialContainerEXT_) &&
           LoadFunction(instance_, "xrDestroySpatialContainerEXT", &xrDestroySpatialContainerEXT_) &&
           LoadFunction(instance_, "xrCreateSpatialContainerSpaceEXT",
                        &xrCreateSpatialContainerSpaceEXT_) &&
           LoadFunction(instance_, "xrRequestSpatialContainerVisibleEXT",
                        &xrRequestSpatialContainerVisibleEXT_) &&
           LoadFunction(instance_, "xrRequestSpatialContainerBoundsModeEXT",
                        &xrRequestSpatialContainerBoundsModeEXT_) &&
           LoadFunction(instance_, "xrGetSpatialContainerBoundsEXT",
                        &xrGetSpatialContainerBoundsEXT_) &&
           LoadFunction(instance_, "xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT",
                        &xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT_) &&
           LoadFunction(instance_, "xrBeginSpatialContainerRenderingEXT",
                        &xrBeginSpatialContainerRenderingEXT_) &&
           LoadFunction(instance_, "xrEndSpatialContainerRenderingEXT",
                        &xrEndSpatialContainerRenderingEXT_) &&
           LoadFunction(instance_, "xrLocateSpatialContainerViewsEXT",
                        &xrLocateSpatialContainerViewsEXT_);
}

bool XrApp::InitializeSystem() {
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &systemInfo, &systemId_));

    XrSystemSpatialContainerPropertiesEXT containerProperties{
            XR_TYPE_SYSTEM_SPATIAL_CONTAINER_PROPERTIES_EXT};
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    systemProperties.next = &containerProperties;
    XR_CHECK(xrGetSystemProperties(instance_, systemId_, &systemProperties));
    LOGI("System %s: max containers %u, bounded %d, immersive %d", systemProperties.systemName,
         containerProperties.maxSpatialContainerCount, containerProperties.supportsBounded,
         containerProperties.supportsImmersive);
    if (!containerProperties.supportsBounded) {
        boundsMode_ = XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT;
    }

    uint32_t count = 0;
    XR_CHECK(xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT_(instance_, systemId_, 0,
                                                                           &count, nullptr));
    std::vector<XrSpatialContainerGraphicsPresentationEXT> presentations(count);
    XR_CHECK(xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT_(
            instance_, systemId_, count, &count, presentations.data()));
    if (std::find(presentations.begin(), presentations.end(),
                  XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT) ==
        presentations.end()) {
        LOGE("Self-rendering spatial containers are not supported");
        return false;
    }

    XR_CHECK(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfigType_, 0, &count,
                                               nullptr));
    configViews_.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfigType_, count,
                                               &count, configViews_.data()));
    if (configViews_.size() != 2) {
        LOGE("Expected a stereo view configuration, got %zu views", configViews_.size());
        return false;
    }

    XR_CHECK(xrEnumerateEnvironmentBlendModes(instance_, systemId_, viewConfigType_, 0, &count,
                                              nullptr));
    std::vector<XrEnvironmentBlendMode> blendModes(count);
    XR_CHECK(xrEnumerateEnvironmentBlendModes(instance_, systemId_, viewConfigType_, count, &count,
                                              blendModes.data()));
    // Prefer modes that keep the real world visible around the container.
    for (XrEnvironmentBlendMode preferred :
         {XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE,
          XR_ENVIRONMENT_BLEND_MODE_OPAQUE}) {
        if (std::find(blendModes.begin(), blendModes.end(), preferred) != blendModes.end()) {
            blendMode_ = preferred;
            break;
        }
    }
    LOGI("Environment blend mode: %d", blendMode_);
    return true;
}

bool XrApp::CreateSession() {
    XrSessionCreateInfoSpatialContainersEXT containersInfo{
            XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT};
    containersInfo.next = renderer_->SessionBinding();
    XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
    createInfo.next = &containersInfo;
    createInfo.systemId = systemId_;
    XR_CHECK(xrCreateSession(instance_, &createInfo, &session_));

    CreateActions();
    return true;
}

void XrApp::CreateActions() {
    XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(actionSetInfo.actionSetName, "container");
    std::strcpy(actionSetInfo.localizedActionSetName, "Container");
    if (XR_FAILED(xrCreateActionSet(instance_, &actionSetInfo, &actionSet_))) return;

    xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]);
    xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]);

    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = handPaths_;
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(actionInfo.actionName, "select");
    std::strcpy(actionInfo.localizedActionName, "Select");
    if (XR_FAILED(xrCreateAction(actionSet_, &actionInfo, &selectAction_))) return;
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(actionInfo.actionName, "aim");
    std::strcpy(actionInfo.localizedActionName, "Aim");
    if (XR_FAILED(xrCreateAction(actionSet_, &actionInfo, &aimAction_))) return;

    auto suggest = [this](const char* profile, const char* selectComponent) {
        XrPath profilePath;
        xrStringToPath(instance_, profile, &profilePath);
        std::vector<XrActionSuggestedBinding> bindings;
        for (const char* hand : {"left", "right"}) {
            XrPath select, aim;
            xrStringToPath(instance_,
                           (std::string("/user/hand/") + hand + "/input/" + selectComponent).c_str(),
                           &select);
            xrStringToPath(instance_, (std::string("/user/hand/") + hand + "/input/aim/pose").c_str(),
                           &aim);
            bindings.push_back({selectAction_, select});
            bindings.push_back({aimAction_, aim});
        }
        XrInteractionProfileSuggestedBinding suggested{
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profilePath;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        if (XR_FAILED(xrSuggestInteractionProfileBindings(instance_, &suggested))) {
            LOGE("Bindings rejected for %s", profile);
        }
    };
    suggest("/interaction_profiles/khr/simple_controller", "select/click");
    if (hasHandInteraction_) {
        suggest("/interaction_profiles/ext/hand_interaction_ext", "pinch_ext/value");
    }

    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = aimAction_;
        spaceInfo.subactionPath = handPaths_[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[hand]);
    }

    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet_;
    xrAttachSessionActionSets(session_, &attachInfo);
}

bool XrApp::CreateContainer() {
    XrSpatialContainerCreateInfoEXT createInfo{XR_TYPE_SPATIAL_CONTAINER_CREATE_INFO_EXT};
    createInfo.graphicsPresentation = XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT;
    createInfo.suggestedBounds = kSuggestedBounds;
    XR_CHECK(xrCreateSpatialContainerEXT_(session_, &createInfo, &container_));

    XrSpatialContainerSpaceCreateInfoEXT spaceInfo{XR_TYPE_SPATIAL_CONTAINER_SPACE_CREATE_INFO_EXT};
    spaceInfo.spatialContainer = container_;
    XR_CHECK(xrCreateSpatialContainerSpaceEXT_(session_, &spaceInfo, &containerSpace_));

    // The bounds mode must be set before the container is made visible.
    RequestBoundsMode(boundsMode_);
    XrSpatialContainerVisibleRequestInfoEXT visibleInfo{
            XR_TYPE_SPATIAL_CONTAINER_VISIBLE_REQUEST_INFO_EXT};
    visibleInfo.visible = XR_TRUE;
    XR_CHECK(xrRequestSpatialContainerVisibleEXT_(container_, &visibleInfo));

    XrSpatialContainerBoundsGetInfoEXT boundsInfo{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_GET_INFO_EXT};
    XrSpatialContainerBoundsEXT bounds{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT};
    if (XR_SUCCEEDED(xrGetSpatialContainerBoundsEXT_(container_, &boundsInfo, &bounds))) {
        bounds_ = bounds.bounds;
        infiniteBounds_ = bounds.infiniteBounds;
    }
    LOGI("Container created: %.2f x %.2f x %.2f m, infinite %d", bounds_.width, bounds_.height,
         bounds_.depth, infiniteBounds_);
    return true;
}

void XrApp::RequestBoundsMode(XrSpatialContainerBoundsModeEXT mode) {
    XrSpatialContainerBoundsModeRequestInfoEXT info{
            XR_TYPE_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_INFO_EXT};
    info.boundsMode = mode;
    const XrResult result = xrRequestSpatialContainerBoundsModeEXT_(container_, &info);
    LOGI("Requested bounds mode %s: %d",
         mode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT ? "immersive" : "bounded", result);
}

void XrApp::PollEvents(bool* exitRequested) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                sessionState_ = changed.state;
                LOGI("Session state %d", sessionState_);
                switch (sessionState_) {
                    // With spatial containers the session is not begun explicitly; frames are
                    // submitted per container (see hello_xr's spatial container program).
                    case XR_SESSION_STATE_IDLE:
                    case XR_SESSION_STATE_READY:
                        sessionRunning_ = true;
                        break;
                    case XR_SESSION_STATE_STOPPING:
                        sessionRunning_ = false;
                        break;
                    case XR_SESSION_STATE_EXITING:
                    case XR_SESSION_STATE_LOSS_PENDING:
                        sessionRunning_ = false;
                        *exitRequested = true;
                        break;
                    default:
                        break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                *exitRequested = true;
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT: {
                const auto& changed =
                        *reinterpret_cast<XrEventDataSpatialContainerVisibleChangedEXT*>(&event);
                containerVisible_ = changed.visible == XR_TRUE;
                LOGI("Container visible: %d", containerVisible_);
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT: {
                const auto& changed =
                        *reinterpret_cast<XrEventDataSpatialContainerInteractableChangedEXT*>(&event);
                containerInteractable_ = changed.interactable == XR_TRUE;
                LOGI("Container interactable: %d", containerInteractable_);
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT: {
                const auto& changed =
                        *reinterpret_cast<XrEventDataSpatialContainerBoundsChangedEXT*>(&event);
                bounds_ = changed.bounds;
                infiniteBounds_ = changed.infiniteBounds == XR_TRUE;
                boundsMode_ = changed.boundsMode;
                immersive_ = boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT;
                // The runtime repeats this event while the container moves; log real changes only.
                constexpr float kLogEpsilon = 0.005f;  // 5 mm
                if (boundsMode_ != lastLoggedBoundsMode_ || infiniteBounds_ != lastLoggedInfinite_ ||
                    std::fabs(bounds_.width - lastLoggedBounds_.width) > kLogEpsilon ||
                    std::fabs(bounds_.height - lastLoggedBounds_.height) > kLogEpsilon ||
                    std::fabs(bounds_.depth - lastLoggedBounds_.depth) > kLogEpsilon) {
                    LOGI("Bounds changed: mode %d, %.2f x %.2f x %.2f m, infinite %d", boundsMode_,
                         bounds_.width, bounds_.height, bounds_.depth, infiniteBounds_);
                    lastLoggedBounds_ = bounds_;
                    lastLoggedBoundsMode_ = boundsMode_;
                    lastLoggedInfinite_ = infiniteBounds_;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_REQUEST_DENIED_EXT:
                LOGI("Visibility request denied");
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_DENIED_EXT:
                LOGI("Bounds mode request denied");
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_CLOSED_EXT:
                LOGI("Container closed by the user");
                containerVisible_ = false;
                containerClosed_ = true;
                break;
            default:
                break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

XrVector3f XrApp::ToggleCubeCenter() const {
    // Front-bottom-right corner of the box when bounded; a fixed spot near the origin when
    // immersive (the bounds are infinite there).
    if (boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT || infiniteBounds_) {
        return {0.35f, -0.25f, 0.2f};
    }
    const float margin = kToggleCubeHalf * 1.5f;
    return {bounds_.width * 0.5f - margin, -bounds_.height * 0.5f + margin,
            bounds_.depth * 0.5f - margin};
}

bool XrApp::AimHitsToggleCube(const XrPosef& aim) const {
    // Slab test of the aim ray (-Z of the aim pose) against the axis-aligned toggle cube.
    const Mat4 pose = Mat4::FromPose(aim);
    const float origin[3] = {aim.position.x, aim.position.y, aim.position.z};
    const float direction[3] = {-pose.m[8], -pose.m[9], -pose.m[10]};
    const XrVector3f center = ToggleCubeCenter();
    const float c[3] = {center.x, center.y, center.z};
    float tMin = 0.0f, tMax = 1e6f;
    for (int axis = 0; axis < 3; ++axis) {
        const float lo = c[axis] - kToggleCubeHalf, hi = c[axis] + kToggleCubeHalf;
        if (std::fabs(direction[axis]) < 1e-6f) {
            if (origin[axis] < lo || origin[axis] > hi) return false;
            continue;
        }
        float t0 = (lo - origin[axis]) / direction[axis];
        float t1 = (hi - origin[axis]) / direction[axis];
        if (t0 > t1) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) return false;
    }
    return true;
}

void XrApp::PollActions(XrTime now) {
    toggleCubeHovered_ = false;
    if (toggleRequested_.exchange(false)) {
        LOGI("Toggle requested from the Compose panel");
        RequestBoundsMode(boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT
                                  ? XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT
                                  : XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT);
    }
    if (actionSet_ == XR_NULL_HANDLE || selectAction_ == XR_NULL_HANDLE) return;
    const XrActiveActionSet activeSet{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    if (XR_FAILED(xrSyncActions(session_, &syncInfo)) || containerSpace_ == XR_NULL_HANDLE) return;

    for (int hand = 0; hand < 2; ++hand) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags kValid =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (aimSpaces_[hand] == XR_NULL_HANDLE ||
            XR_FAILED(xrLocateSpace(aimSpaces_[hand], containerSpace_, now, &location)) ||
            (location.locationFlags & kValid) != kValid || !AimHitsToggleCube(location.pose)) {
            continue;
        }
        toggleCubeHovered_ = true;

        // Pinch or select while aiming at the toggle cube switches the bounds mode. Pinches
        // anywhere else are left alone, so grabbing the container's handle still moves it.
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = selectAction_;
        getInfo.subactionPath = handPaths_[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &state)) && state.isActive &&
            state.changedSinceLastSync && state.currentState) {
            LOGI("Toggle cube picked with the %s hand", hand == 0 ? "left" : "right");
            RequestBoundsMode(boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT
                                      ? XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT
                                      : XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT);
        }
    }
}

void XrApp::UpdateRenderingState() {
    if (containerVisible_ && !containerRendering_) {
        XrSpatialContainerBeginInfoEXT beginInfo{XR_TYPE_SPATIAL_CONTAINER_BEGIN_INFO_EXT};
        beginInfo.spatialContainer = container_;
        beginInfo.primaryViewConfigurationType = viewConfigType_;
        if (XR_SUCCEEDED(xrBeginSpatialContainerRenderingEXT_(session_, &beginInfo))) {
            containerRendering_ = true;
            LOGI("Container rendering started");
        }
    } else if (!containerVisible_ && containerRendering_) {
        XrSpatialContainerEndInfoEXT endInfo{XR_TYPE_SPATIAL_CONTAINER_END_INFO_EXT};
        endInfo.spatialContainer = container_;
        xrEndSpatialContainerRenderingEXT_(session_, &endInfo);
        containerRendering_ = false;
        LOGI("Container rendering stopped");
    }
    if (containerClosed_ && container_ != XR_NULL_HANDLE) {
        renderer_->DestroySwapchains();
        xrDestroySpace(containerSpace_);
        containerSpace_ = XR_NULL_HANDLE;
        xrDestroySpatialContainerEXT_(container_);
        container_ = XR_NULL_HANDLE;
        jclass cls = env_->GetObjectClass(activity_);
        env_->CallVoidMethod(activity_, env_->GetMethodID(cls, "finish", "()V"));
        env_->DeleteLocalRef(cls);
    }
}

void XrApp::RenderFrame() {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, &waitInfo, &frameState))) return;

    PollActions(frameState.predictedDisplayTime);
    UpdateRenderingState();

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(session_, &beginInfo))) return;

    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrSpatialContainerCompositionLayerViewConfigurationEXT layerViewConfig{
            XR_TYPE_SPATIAL_CONTAINER_COMPOSITION_LAYER_VIEW_CONFIGURATION_EXT};
    XrCompositionLayerProjectionView projectionViews[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t layerCount = 0;

    if (containerRendering_) {
        XrSpatialContainerViewLocateInfoEXT viewLocateInfo{
                XR_TYPE_SPATIAL_CONTAINER_VIEW_LOCATE_INFO_EXT};
        viewLocateInfo.viewConfigurationType = viewConfigType_;
        viewLocateInfo.space = containerSpace_;
        viewLocateInfo.spatialContainer = container_;
        XrSpatialContainerViewsLocateInfoEXT locateInfo{
                XR_TYPE_SPATIAL_CONTAINER_VIEWS_LOCATE_INFO_EXT};
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.viewLocateInfoCount = 1;
        locateInfo.viewLocateInfos = &viewLocateInfo;
        XrSpatialContainerViewStateEXT viewState{XR_TYPE_SPATIAL_CONTAINER_VIEW_STATE_EXT};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

        constexpr XrViewStateFlags kPoseValid =
                XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        // frameState.shouldRender is not used: with spatial containers the session stays IDLE
        // (it is never begun), so the per-container shouldSubmitLayers decides instead.
        const XrResult locateResult =
                xrLocateSpatialContainerViewsEXT_(session_, &locateInfo, 1, &viewState, 2, views);
        const bool canSubmit = XR_SUCCEEDED(locateResult) && viewState.shouldSubmitLayers &&
                               (viewState.viewStateFlags & kPoseValid) == kPoseValid;
        if (canSubmit != lastCanSubmit_) {
            LOGI("Container views: result %d, shouldSubmitLayers %d, flags 0x%llx, extent %dx%d",
                 locateResult, viewState.shouldSubmitLayers,
                 static_cast<unsigned long long>(viewState.viewStateFlags),
                 viewState.recommendedImageExtent.width, viewState.recommendedImageExtent.height);
            lastCanSubmit_ = canSubmit;
        }
        if (canSubmit) {
            uint32_t eyeWidth = viewState.recommendedImageExtent.width;
            uint32_t eyeHeight = viewState.recommendedImageExtent.height;
            if (eyeWidth == 0 || eyeHeight == 0) {
                eyeWidth = configViews_[0].recommendedImageRectWidth;
                eyeHeight = configViews_[0].recommendedImageRectHeight;
            }

            if (renderer_->EnsureSwapchains(session_, eyeWidth, eyeHeight)) {
                SceneInput input{};
                input.timeSeconds =
                        static_cast<float>(frameState.predictedDisplayTime % 60'000'000'000LL) * 1e-9f;
                input.bounds = bounds_;
                input.infiniteBounds = infiniteBounds_;
                // The mode cues follow the runtime-confirmed bounds mode (BOUNDS_CHANGED), not the
                // request, so a denied request leaves the cube unchanged.
                input.immersive = boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT;
                input.interactable = containerInteractable_;
                input.toggleCenter = ToggleCubeCenter();
                input.toggleHalfExtent = kToggleCubeHalf;
                input.toggleHovered = toggleCubeHovered_;
                const SceneState scene = BuildScene(input);

                EyeView eyes[2];
                BuildEyeViews(views, eyeWidth, eyeHeight, eyes);
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    projectionViews[eye].pose = views[eye].pose;
                    projectionViews[eye].fov = views[eye].fov;
                }
                if (renderer_->Render(eyes, scene, projectionViews)) {
                    layerViewConfig.viewConfigurationType = viewConfigType_;
                    projectionLayer.next = &layerViewConfig;
                    projectionLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    projectionLayer.space = containerSpace_;
                    projectionLayer.viewCount = 2;
                    projectionLayer.views = projectionViews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
                    layerCount = 1;
                }
            }
        }
    }

    XrSpatialContainerLayerVolumeClippingEXT clipping{
            XR_TYPE_SPATIAL_CONTAINER_LAYER_VOLUME_CLIPPING_EXT};
    clipping.volumeClipping = XR_SPATIAL_CONTAINER_VOLUME_CLIPPING_STRICT_EXT;
    XrSpatialContainerLayerEXT containerLayer{XR_TYPE_SPATIAL_CONTAINER_LAYER_EXT};
    containerLayer.next = &clipping;
    containerLayer.spatialContainer = container_;
    containerLayer.retainPreviousSubmission = layerCount == 0 ? XR_TRUE : XR_FALSE;
    containerLayer.layerCount = layerCount;
    containerLayer.layers = layerCount > 0 ? layers : nullptr;

    XrSpatialContainerLayerFrameEndInfoEXT containerEndInfo{
            XR_TYPE_SPATIAL_CONTAINER_LAYER_FRAME_END_INFO_EXT};
    containerEndInfo.containerLayerCount = containerRendering_ ? 1 : 0;
    containerEndInfo.containerLayers = containerRendering_ ? &containerLayer : nullptr;

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.next = &containerEndInfo;
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = blendMode_;
    endInfo.layerCount = 0;  // Container content is submitted through containerEndInfo.
    const XrResult result = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(result)) LOGE("xrEndFrame failed: %d", result);
}
