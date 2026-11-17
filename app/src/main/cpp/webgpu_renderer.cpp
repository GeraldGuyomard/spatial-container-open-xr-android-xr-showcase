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

// WebGPU (Dawn) backend. Dawn creates its own Vulkan device and cannot render into OpenXR
// swapchain images, so color goes through XR_KHR_android_surface_swapchain (an Android Surface that
// Dawn presents to). OpenXR still requires a graphics binding for the session: a 1x1 pbuffer EGL
// context is created for that purpose only and never draws. Surface swapchains carry no depth, so
// this backend submits no layer depth.

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <string>
#include <vector>

#include "log.h"
#include "renderer.h"

namespace {

constexpr char kShader[] = R"(
struct Uniforms {
    mvp : mat4x4f,
    model : mat4x4f,
    tint : vec4f,
};
@group(0) @binding(0) var<uniform> u : Uniforms;

struct VertexIn {
    @location(0) position : vec3f,
    @location(1) normal : vec3f,
    @location(2) color : vec3f,
};

struct VertexOut {
    @builtin(position) position : vec4f,
    @location(0) normal : vec3f,
    @location(1) color : vec3f,
    @location(2) objectNormal : vec3f,
};

@vertex fn vs_main(in : VertexIn) -> VertexOut {
    var out : VertexOut;
    out.position = u.mvp * vec4f(in.position, 1.0);
    out.normal = (u.model * vec4f(in.normal, 0.0)).xyz;
    out.color = in.color;
    out.objectNormal = in.normal;
    return out;
}

// Keep in sync with shaders/lit.frag (Vulkan backend).
@fragment fn fs_lit(in : VertexOut) -> @location(0) vec4f {
    let light = normalize(vec3f(0.4, 0.8, 0.5));
    let diffuse = 0.35 + 0.65 * max(dot(normalize(in.normal), light), 0.0);
    // tint.w selects the palette: 0 = bounded (cool blues), 1 = immersive (warm oranges),
    // 2 = plain white (the toggle cube).
    // Each face gets its own shade from its object-space normal so the faces stay distinct.
    let f = in.objectNormal * 0.5 + 0.5;
    let cool = vec3f(0.10 + 0.15 * f.z, 0.40 + 0.35 * f.x, 0.70 + 0.30 * f.y);
    let warm = vec3f(0.95, 0.30 + 0.40 * f.x, 0.05 + 0.20 * f.y + 0.10 * f.z);
    let base = select(select(cool, warm, u.tint.w > 0.5), vec3f(0.95), u.tint.w > 1.5);
    return vec4f(base * diffuse * u.tint.rgb, 1.0);
}

@fragment fn fs_unlit(in : VertexOut) -> @location(0) vec4f {
    return vec4f(in.color * u.tint.rgb, 1.0);
}
)";

template <typename T>
bool LoadXrFunction(XrInstance instance, const char* name, T* function) {
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name,
                                        reinterpret_cast<PFN_xrVoidFunction*>(function)))) {
        LOGE("Missing OpenXR function %s", name);
        return false;
    }
    return true;
}

wgpu::Buffer CreateBuffer(const wgpu::Device& device, wgpu::BufferUsage usage, const void* data,
                          uint64_t size) {
    wgpu::BufferDescriptor descriptor{};
    descriptor.usage = usage | wgpu::BufferUsage::CopyDst;
    descriptor.size = (size + 3) & ~3ull;
    wgpu::Buffer buffer = device.CreateBuffer(&descriptor);
    if (data != nullptr) device.GetQueue().WriteBuffer(buffer, 0, data, descriptor.size);
    return buffer;
}

class WebGpuRenderer final : public Renderer {
public:
    explicit WebGpuRenderer(JavaVM* vm) : vm_(vm) {}
    ~WebGpuRenderer() override;

    const char* Name() const override { return "WebGPU"; }
    bool SelectInstanceExtensions(const std::vector<std::string>& available,
                                  std::vector<const char*>* enabled) override;
    bool Initialize(XrInstance instance, XrSystemId systemId) override;
    const void* SessionBinding() const override { return &binding_; }
    bool EnsureSwapchains(XrSession session, uint32_t eyeWidth, uint32_t eyeHeight) override;
    void DestroySwapchains() override;
    bool Render(const EyeView (&eyes)[2], const SceneState& scene,
                XrCompositionLayerProjectionView (&views)[2]) override;

private:
    bool InitializeDawn();
    bool CreateEglContext();
    bool ConfigureSurface(ANativeWindow* window, uint32_t width, uint32_t height);
    void ReleaseSurface();
    void CreatePipelines();

    JavaVM* vm_;

    // EGL context used only to satisfy the session graphics binding.
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    EGLConfig eglConfig_ = nullptr;
    XrGraphicsBindingOpenGLESAndroidKHR binding_{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};

    PFN_xrCreateSwapchainAndroidSurfaceKHR xrCreateSwapchainAndroidSurfaceKHR_ = nullptr;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    jobject surfaceRef_ = nullptr;  // Global ref to the swapchain's Java Surface.
    uint32_t eyeWidth_ = 0;
    uint32_t eyeHeight_ = 0;
    bool swapchainFailed_ = false;

    wgpu::Instance instance_;
    wgpu::Adapter adapter_;
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Surface surface_;
    ANativeWindow* window_ = nullptr;
    wgpu::TextureFormat surfaceFormat_ = wgpu::TextureFormat::Undefined;
    wgpu::Texture depthTexture_;

    wgpu::RenderPipeline litPipeline_;
    wgpu::RenderPipeline linePipeline_;
    wgpu::BindGroupLayout bindGroupLayout_;
    wgpu::BindGroup bindGroup_;
    wgpu::Buffer uniformBuffer_;
    wgpu::Buffer cubeVertexBuffer_;
    wgpu::Buffer cubeIndexBuffer_;
    wgpu::Buffer lineVertexBuffer_;
    uint32_t cubeIndexCount_ = 0;
    uint32_t lineVertexCount_ = 0;
};

WebGpuRenderer::~WebGpuRenderer() {
    DestroySwapchains();
    // Release Dawn objects before the EGL context, in reverse creation order.
    litPipeline_ = nullptr;
    linePipeline_ = nullptr;
    bindGroup_ = nullptr;
    bindGroupLayout_ = nullptr;
    uniformBuffer_ = nullptr;
    cubeVertexBuffer_ = nullptr;
    cubeIndexBuffer_ = nullptr;
    lineVertexBuffer_ = nullptr;
    queue_ = nullptr;
    device_ = nullptr;
    adapter_ = nullptr;
    instance_ = nullptr;
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
        if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
        eglTerminate(eglDisplay_);
    }
}

bool WebGpuRenderer::SelectInstanceExtensions(const std::vector<std::string>& available,
                                              std::vector<const char*>* enabled) {
    for (const char* name :
         {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME}) {
        if (std::find(available.begin(), available.end(), name) == available.end()) {
            LOGE("WebGPU backend: required extension %s is not supported", name);
            return false;
        }
        enabled->push_back(name);
    }
    return true;
}

bool WebGpuRenderer::Initialize(XrInstance instance, XrSystemId systemId) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!LoadXrFunction(instance, "xrGetOpenGLESGraphicsRequirementsKHR", &getRequirements) ||
        !LoadXrFunction(instance, "xrCreateSwapchainAndroidSurfaceKHR",
                        &xrCreateSwapchainAndroidSurfaceKHR_)) {
        return false;
    }
    // Required before xrCreateSession, even though GL never renders.
    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (XR_FAILED(getRequirements(instance, systemId, &requirements))) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR failed");
        return false;
    }
    if (!CreateEglContext() || !InitializeDawn()) return false;
    binding_.display = eglDisplay_;
    binding_.config = eglConfig_;
    binding_.context = eglContext_;
    return true;
}

bool WebGpuRenderer::CreateEglContext() {
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(eglDisplay_, nullptr, nullptr)) return false;

    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                    EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) ||
        numConfigs == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, surfaceAttribs);
    if (eglContext_ == EGL_NO_CONTEXT || eglSurface_ == EGL_NO_SURFACE ||
        !eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LOGE("Failed to create the EGL context for the session binding");
        return false;
    }
    return true;
}

bool WebGpuRenderer::InitializeDawn() {
    static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instanceDescriptor{};
    instanceDescriptor.requiredFeatureCount = 1;
    instanceDescriptor.requiredFeatures = &kTimedWaitAny;
    instance_ = wgpu::CreateInstance(&instanceDescriptor);
    if (instance_ == nullptr) {
        LOGE("wgpu::CreateInstance failed");
        return false;
    }

    wgpu::RequestAdapterOptions adapterOptions{};
    adapterOptions.backendType = wgpu::BackendType::Vulkan;
    instance_.WaitAny(
            instance_.RequestAdapter(
                    &adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                    [this](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
                           wgpu::StringView message) {
                        if (status != wgpu::RequestAdapterStatus::Success) {
                            LOGE("RequestAdapter failed: %s", std::string(message).c_str());
                            return;
                        }
                        adapter_ = std::move(adapter);
                    }),
            UINT64_MAX);
    if (adapter_ == nullptr) return false;

    wgpu::AdapterInfo info;
    adapter_.GetInfo(&info);
    LOGI("WebGPU adapter: %s", std::string(info.device).c_str());

    wgpu::DeviceDescriptor deviceDescriptor{};
    deviceDescriptor.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                // Destroyed is the normal teardown when switching backends or quitting.
                if (reason == wgpu::DeviceLostReason::Destroyed) return;
                LOGE("WebGPU device lost (%d): %s", static_cast<int>(reason),
                     std::string(message).c_str());
            });
    deviceDescriptor.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
                LOGE("WebGPU error (%d): %s", static_cast<int>(type), std::string(message).c_str());
            });
    instance_.WaitAny(
            adapter_.RequestDevice(
                    &deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                    [this](wgpu::RequestDeviceStatus status, wgpu::Device device,
                           wgpu::StringView message) {
                        if (status != wgpu::RequestDeviceStatus::Success) {
                            LOGE("RequestDevice failed: %s", std::string(message).c_str());
                            return;
                        }
                        device_ = std::move(device);
                    }),
            UINT64_MAX);
    if (device_ == nullptr) return false;
    queue_ = device_.GetQueue();

    std::vector<Vertex> cubeVertices;
    std::vector<uint16_t> cubeIndices;
    BuildCube(&cubeVertices, &cubeIndices);
    cubeIndexCount_ = static_cast<uint32_t>(cubeIndices.size());
    cubeVertexBuffer_ = CreateBuffer(device_, wgpu::BufferUsage::Vertex, cubeVertices.data(),
                                     cubeVertices.size() * sizeof(Vertex));
    cubeIndexBuffer_ = CreateBuffer(device_, wgpu::BufferUsage::Index, cubeIndices.data(),
                                    cubeIndices.size() * sizeof(uint16_t));
    const std::vector<Vertex> lines = BuildBoxEdges();
    lineVertexCount_ = static_cast<uint32_t>(lines.size());
    lineVertexBuffer_ = CreateBuffer(device_, wgpu::BufferUsage::Vertex, lines.data(),
                                     lines.size() * sizeof(Vertex));
    uniformBuffer_ =
            CreateBuffer(device_, wgpu::BufferUsage::Uniform, nullptr, kUniformBufferSize);

    wgpu::BindGroupLayoutEntry layoutEntry{};
    layoutEntry.binding = 0;
    layoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    layoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    layoutEntry.buffer.hasDynamicOffset = true;
    layoutEntry.buffer.minBindingSize = sizeof(Uniforms);
    wgpu::BindGroupLayoutDescriptor layoutDescriptor{};
    layoutDescriptor.entryCount = 1;
    layoutDescriptor.entries = &layoutEntry;
    bindGroupLayout_ = device_.CreateBindGroupLayout(&layoutDescriptor);

    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = uniformBuffer_;
    entry.size = sizeof(Uniforms);
    wgpu::BindGroupDescriptor bindGroupDescriptor{};
    bindGroupDescriptor.layout = bindGroupLayout_;
    bindGroupDescriptor.entryCount = 1;
    bindGroupDescriptor.entries = &entry;
    bindGroup_ = device_.CreateBindGroup(&bindGroupDescriptor);
    return true;
}

void WebGpuRenderer::CreatePipelines() {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor shaderDescriptor{};
    shaderDescriptor.nextInChain = &wgsl;
    wgpu::ShaderModule shader = device_.CreateShaderModule(&shaderDescriptor);

    wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{};
    pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
    pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout_;
    wgpu::PipelineLayout pipelineLayout = device_.CreatePipelineLayout(&pipelineLayoutDescriptor);

    wgpu::VertexAttribute attributes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        attributes[i].format = wgpu::VertexFormat::Float32x3;
        attributes[i].offset = i * 3 * sizeof(float);
        attributes[i].shaderLocation = i;
    }
    wgpu::VertexBufferLayout vertexLayout{};
    vertexLayout.arrayStride = sizeof(Vertex);
    vertexLayout.attributeCount = 3;
    vertexLayout.attributes = attributes;

    wgpu::ColorTargetState colorTarget{};
    colorTarget.format = surfaceFormat_;

    wgpu::DepthStencilState depthStencil{};
    depthStencil.format = wgpu::TextureFormat::Depth24Plus;
    depthStencil.depthWriteEnabled = wgpu::OptionalBool::True;
    depthStencil.depthCompare = wgpu::CompareFunction::Less;

    auto create = [&](const char* fragmentEntry, wgpu::PrimitiveTopology topology) {
        wgpu::FragmentState fragment{};
        fragment.module = shader;
        fragment.entryPoint = fragmentEntry;
        fragment.targetCount = 1;
        fragment.targets = &colorTarget;

        wgpu::RenderPipelineDescriptor descriptor{};
        descriptor.layout = pipelineLayout;
        descriptor.vertex.module = shader;
        descriptor.vertex.entryPoint = "vs_main";
        descriptor.vertex.bufferCount = 1;
        descriptor.vertex.buffers = &vertexLayout;
        descriptor.fragment = &fragment;
        descriptor.primitive.topology = topology;
        descriptor.primitive.cullMode = topology == wgpu::PrimitiveTopology::TriangleList
                                                ? wgpu::CullMode::Back
                                                : wgpu::CullMode::None;
        descriptor.depthStencil = &depthStencil;
        return device_.CreateRenderPipeline(&descriptor);
    };
    litPipeline_ = create("fs_lit", wgpu::PrimitiveTopology::TriangleList);
    linePipeline_ = create("fs_unlit", wgpu::PrimitiveTopology::LineList);
}

bool WebGpuRenderer::EnsureSwapchains(XrSession session, uint32_t eyeWidth, uint32_t eyeHeight) {
    if (swapchain_ != XR_NULL_HANDLE && eyeWidth == eyeWidth_ && eyeHeight == eyeHeight_) {
        return true;
    }
    DestroySwapchains();
    if (swapchainFailed_) return false;
    swapchainFailed_ = true;  // Cleared on success, so a failure is not retried every frame.

    // Both eyes share one surface, side by side. The surface's producer (Dawn) defines the
    // image format, so the runtime requires the format-related fields to be zero.
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.width = eyeWidth * 2;
    createInfo.height = eyeHeight;
    jobject surface = nullptr;
    const XrResult result =
            xrCreateSwapchainAndroidSurfaceKHR_(session, &createInfo, &swapchain_, &surface);
    if (XR_FAILED(result)) {
        LOGE("xrCreateSwapchainAndroidSurfaceKHR failed: %d", result);
        swapchain_ = XR_NULL_HANDLE;
        return false;
    }
    JNIEnv* env = nullptr;
    vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);  // Attached by XrApp.
    surfaceRef_ = env->NewGlobalRef(surface);

    ANativeWindow* window = ANativeWindow_fromSurface(env, surfaceRef_);
    const bool configured = ConfigureSurface(window, createInfo.width, createInfo.height);
    ANativeWindow_release(window);
    if (!configured) return false;

    eyeWidth_ = eyeWidth;
    eyeHeight_ = eyeHeight;
    swapchainFailed_ = false;
    LOGI("WebGPU: surface swapchain %ux%u per eye (no layer depth)", eyeWidth, eyeHeight);
    return true;
}

void WebGpuRenderer::DestroySwapchains() {
    ReleaseSurface();
    if (swapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
    }
    if (surfaceRef_ != nullptr) {
        JNIEnv* env = nullptr;
        vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env != nullptr) env->DeleteGlobalRef(surfaceRef_);
        surfaceRef_ = nullptr;
    }
    eyeWidth_ = eyeHeight_ = 0;
}

bool WebGpuRenderer::ConfigureSurface(ANativeWindow* window, uint32_t width, uint32_t height) {
    ReleaseSurface();
    ANativeWindow_acquire(window);
    window_ = window;

    wgpu::SurfaceSourceAndroidNativeWindow source{};
    source.window = window;
    wgpu::SurfaceDescriptor surfaceDescriptor{};
    surfaceDescriptor.nextInChain = &source;
    surface_ = instance_.CreateSurface(&surfaceDescriptor);
    if (surface_ == nullptr) {
        LOGE("Failed to create the WebGPU surface");
        ReleaseSurface();
        return false;
    }

    wgpu::SurfaceCapabilities capabilities;
    surface_.GetCapabilities(adapter_, &capabilities);
    if (capabilities.formatCount == 0) {
        LOGE("Surface reports no formats");
        ReleaseSurface();
        return false;
    }
    const wgpu::TextureFormat format = capabilities.formats[0];
    wgpu::CompositeAlphaMode alphaMode = capabilities.alphaModes[0];
    for (size_t i = 0; i < capabilities.alphaModeCount; ++i) {
        if (capabilities.alphaModes[i] == wgpu::CompositeAlphaMode::Premultiplied) {
            alphaMode = wgpu::CompositeAlphaMode::Premultiplied;
        }
    }

    wgpu::SurfaceConfiguration config{};
    config.device = device_;
    config.format = format;
    config.width = width;
    config.height = height;
    config.alphaMode = alphaMode;
    config.presentMode = wgpu::PresentMode::Fifo;
    surface_.Configure(&config);
    LOGI("WebGPU: surface configured %ux%u, format %d, alpha mode %d", width, height,
         static_cast<int>(format), static_cast<int>(alphaMode));

    if (format != surfaceFormat_ || litPipeline_ == nullptr) {
        surfaceFormat_ = format;
        CreatePipelines();
    }

    wgpu::TextureDescriptor depthDescriptor{};
    depthDescriptor.size = {width, height, 1};
    depthDescriptor.format = wgpu::TextureFormat::Depth24Plus;
    depthDescriptor.usage = wgpu::TextureUsage::RenderAttachment;
    depthTexture_ = device_.CreateTexture(&depthDescriptor);
    return true;
}

void WebGpuRenderer::ReleaseSurface() {
    if (surface_ != nullptr) {
        surface_.Unconfigure();
        surface_ = nullptr;
    }
    depthTexture_ = nullptr;
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

bool WebGpuRenderer::Render(const EyeView (&eyes)[2], const SceneState& scene,
                            XrCompositionLayerProjectionView (&views)[2]) {
    if (surface_ == nullptr) return false;

    uint8_t uniformData[kUniformBufferSize];
    PackUniforms(eyes, scene, uniformData);
    queue_.WriteBuffer(uniformBuffer_, 0, uniformData, sizeof(uniformData));

    wgpu::SurfaceTexture surfaceTexture;
    surface_.GetCurrentTexture(&surfaceTexture);
    if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        LOGE("GetCurrentTexture failed: %d", static_cast<int>(surfaceTexture.status));
        return false;
    }

    wgpu::RenderPassColorAttachment colorAttachment{};
    colorAttachment.view = surfaceTexture.texture.CreateView();
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;
    colorAttachment.clearValue = {0, 0, 0, 0};  // Transparent around the content.
    wgpu::RenderPassDepthStencilAttachment depthAttachment{};
    depthAttachment.view = depthTexture_.CreateView();
    depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
    depthAttachment.depthStoreOp = wgpu::StoreOp::Discard;
    depthAttachment.depthClearValue = 1.0f;
    wgpu::RenderPassDescriptor passDescriptor{};
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &colorAttachment;
    passDescriptor.depthStencilAttachment = &depthAttachment;

    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const EyeView& view = eyes[eye];
        pass.SetViewport(static_cast<float>(view.viewportX), static_cast<float>(view.viewportY),
                         static_cast<float>(view.viewportWidth),
                         static_cast<float>(view.viewportHeight), 0.0f, 1.0f);
        pass.SetScissorRect(view.viewportX, view.viewportY, view.viewportWidth,
                            view.viewportHeight);

        uint32_t offset = UniformOffset(eye, kSlotCube);
        pass.SetPipeline(litPipeline_);
        pass.SetBindGroup(0, bindGroup_, 1, &offset);
        pass.SetVertexBuffer(0, cubeVertexBuffer_);
        pass.SetIndexBuffer(cubeIndexBuffer_, wgpu::IndexFormat::Uint16);
        pass.DrawIndexed(cubeIndexCount_);

        offset = UniformOffset(eye, kSlotToggle);
        pass.SetBindGroup(0, bindGroup_, 1, &offset);
        pass.DrawIndexed(cubeIndexCount_);

        if (scene.showBounds) {
            offset = UniformOffset(eye, kSlotBounds);
            pass.SetPipeline(linePipeline_);
            pass.SetBindGroup(0, bindGroup_, 1, &offset);
            pass.SetVertexBuffer(0, lineVertexBuffer_);
            pass.Draw(lineVertexCount_);
        }
    }
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
    surface_.Present();

    for (uint32_t eye = 0; eye < 2; ++eye) {
        views[eye].subImage.swapchain = swapchain_;
        views[eye].subImage.imageRect.offset = {static_cast<int32_t>(eyes[eye].viewportX), 0};
        views[eye].subImage.imageRect.extent = {static_cast<int32_t>(eyes[eye].viewportWidth),
                                                static_cast<int32_t>(eyes[eye].viewportHeight)};
        views[eye].subImage.imageArrayIndex = 0;
    }
    return true;
}

}  // namespace

std::unique_ptr<Renderer> CreateWebGpuRenderer(JavaVM* vm) {
    return std::make_unique<WebGpuRenderer>(vm);
}
