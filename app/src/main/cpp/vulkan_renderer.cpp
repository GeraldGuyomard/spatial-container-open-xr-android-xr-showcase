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

// Direct Vulkan backend through XR_KHR_vulkan_enable2: the runtime creates the VkInstance/VkDevice
// with us, owns the color and depth swapchain images, and receives the depth through
// XR_KHR_composition_layer_depth. This is the standard OpenXR rendering path, with no GL or
// Android surface involved.

#include <jni.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "renderer.h"

namespace {

// SPIR-V generated at build time by glslc (see CMakeLists.txt).
constexpr uint32_t kSceneVert[] = {
#include "scene.vert.inc"
};
constexpr uint32_t kLitFrag[] = {
#include "lit.frag.inc"
};
constexpr uint32_t kUnlitFrag[] = {
#include "unlit.frag.inc"
};

#define VK_CHECK(call)                                                          \
    do {                                                                        \
        VkResult vkResult_ = (call);                                            \
        if (vkResult_ != VK_SUCCESS) {                                          \
            LOGE("%s failed: %d (%s:%d)", #call, vkResult_, __FILE__, __LINE__); \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define XR_CHECK(call)                                                          \
    do {                                                                        \
        XrResult xrResult_ = (call);                                            \
        if (XR_FAILED(xrResult_)) {                                             \
            LOGE("%s failed: %d (%s:%d)", #call, xrResult_, __FILE__, __LINE__); \
            return false;                                                       \
        }                                                                       \
    } while (0)

template <typename T>
bool LoadXrFunction(XrInstance instance, const char* name, T* function) {
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name,
                                        reinterpret_cast<PFN_xrVoidFunction*>(function)))) {
        LOGE("Missing OpenXR function %s", name);
        return false;
    }
    return true;
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

struct SwapchainImages {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
};

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override;

    const char* Name() const override { return "Vulkan"; }
    bool SelectInstanceExtensions(const std::vector<std::string>& available,
                                  std::vector<const char*>* enabled) override;
    bool Initialize(XrInstance instance, XrSystemId systemId) override;
    const void* SessionBinding() const override { return &binding_; }
    bool EnsureSwapchains(XrSession session, uint32_t eyeWidth, uint32_t eyeHeight) override;
    void DestroySwapchains() override;
    bool Render(const EyeView (&eyes)[2], const SceneState& scene,
                XrCompositionLayerProjectionView (&views)[2]) override;

private:
    bool CreateDevice(XrInstance instance, XrSystemId systemId);
    bool CreateResources();
    bool CreateBuffer(VkBufferUsageFlags usage, VkDeviceSize size, const void* data, Buffer* out);
    void DestroyBuffer(Buffer* buffer);
    bool CreateSwapchain(XrSession session, const std::vector<int64_t>& formats,
                         const std::vector<VkFormat>& preferred, XrSwapchainUsageFlags usage,
                         VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                         SwapchainImages* out);
    void DestroySwapchain(SwapchainImages* swapchain);
    bool CreateRenderPassAndPipelines(VkFormat colorFormat, VkFormat depthFormat);
    void DestroyRenderPassAndPipelines();
    VkShaderModule CreateShaderModule(const uint32_t* code, size_t size);

    bool layerDepthEnabled_ = false;
    bool loggedDepth_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    Buffer cubeVertices_, cubeIndices_, lineVertices_, uniforms_;
    uint32_t cubeIndexCount_ = 0;
    uint32_t lineVertexCount_ = 0;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipeline litPipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    VkFormat pipelineColorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat pipelineDepthFormat_ = VK_FORMAT_UNDEFINED;

    SwapchainImages color_, depth_;
    // One framebuffer per (color image, depth image) pair: the two swapchains are acquired
    // independently, so their indices are not guaranteed to match.
    std::vector<VkFramebuffer> framebuffers_;
    uint32_t eyeWidth_ = 0;
    uint32_t eyeHeight_ = 0;
    bool swapchainFailed_ = false;

    XrCompositionLayerDepthInfoKHR depthInfos_[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                     {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
};

VulkanRenderer::~VulkanRenderer() {
    DestroySwapchains();
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        DestroyRenderPassAndPipelines();
        if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (setLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        for (Buffer* buffer : {&cubeVertices_, &cubeIndices_, &lineVertices_, &uniforms_}) {
            DestroyBuffer(buffer);
        }
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

bool VulkanRenderer::SelectInstanceExtensions(const std::vector<std::string>& available,
                                              std::vector<const char*>* enabled) {
    auto has = [&](const char* name) {
        return std::find(available.begin(), available.end(), name) != available.end();
    };
    if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        LOGE("Vulkan backend: %s is not supported", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return false;
    }
    enabled->push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    layerDepthEnabled_ = has(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (layerDepthEnabled_) enabled->push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    LOGI("Vulkan: layer depth (%s) %s", XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
         layerDepthEnabled_ ? "enabled" : "not supported");
    return true;
}

bool VulkanRenderer::Initialize(XrInstance instance, XrSystemId systemId) {
    return CreateDevice(instance, systemId) && CreateResources();
}

bool VulkanRenderer::CreateDevice(XrInstance instance, XrSystemId systemId) {
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    if (!LoadXrFunction(instance, "xrGetVulkanGraphicsRequirements2KHR", &getRequirements) ||
        !LoadXrFunction(instance, "xrCreateVulkanInstanceKHR", &createInstance) ||
        !LoadXrFunction(instance, "xrGetVulkanGraphicsDevice2KHR", &getDevice) ||
        !LoadXrFunction(instance, "xrCreateVulkanDeviceKHR", &createDevice)) {
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(getRequirements(instance, systemId, &requirements));

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "SpatialContainerDemo";
    appInfo.pEngineName = "SpatialContainerDemo";
    appInfo.apiVersion = VK_API_VERSION_1_1;  // Negative viewport height is core in 1.1.
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = systemId;
    xrInstanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    XR_CHECK(createInstance(instance, &xrInstanceInfo, &instance_, &vkResult));
    VK_CHECK(vkResult);

    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = systemId;
    deviceGetInfo.vulkanInstance = instance_;
    XR_CHECK(getDevice(instance, &deviceGetInfo, &physicalDevice_));

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    LOGI("Vulkan: device %s, API %u.%u", properties.deviceName,
         VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
    bool found = false;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily_ = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOGE("Vulkan: no graphics queue family");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = &features;
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = systemId;
    xrDeviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrDeviceInfo.vulkanPhysicalDevice = physicalDevice_;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    XR_CHECK(createDevice(instance, &xrDeviceInfo, &device_, &vkResult));
    VK_CHECK(vkResult);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    binding_.instance = instance_;
    binding_.physicalDevice = physicalDevice_;
    binding_.device = device_;
    binding_.queueFamilyIndex = queueFamily_;
    binding_.queueIndex = 0;
    return true;
}

bool VulkanRenderer::CreateBuffer(VkBufferUsageFlags usage, VkDeviceSize size, const void* data,
                                  Buffer* out) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    VK_CHECK(vkCreateBuffer(device_, &info, nullptr, &out->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, out->buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memory);
    // Small, CPU-written every frame: host-visible coherent memory is enough.
    const VkMemoryPropertyFlags wanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        LOGE("Vulkan: no host-visible memory type");
        return false;
    }
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = typeIndex;
    VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &out->memory));
    VK_CHECK(vkBindBufferMemory(device_, out->buffer, out->memory, 0));
    VK_CHECK(vkMapMemory(device_, out->memory, 0, size, 0, &out->mapped));
    if (data != nullptr) std::memcpy(out->mapped, data, size);
    return true;
}

void VulkanRenderer::DestroyBuffer(Buffer* buffer) {
    if (buffer->buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer->buffer, nullptr);
    if (buffer->memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer->memory, nullptr);
    *buffer = {};
}

bool VulkanRenderer::CreateResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cmdInfo, &commandBuffer_));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &fence_));

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    BuildCube(&vertices, &indices);
    cubeIndexCount_ = static_cast<uint32_t>(indices.size());
    const std::vector<Vertex> lines = BuildBoxEdges();
    lineVertexCount_ = static_cast<uint32_t>(lines.size());
    if (!CreateBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.size() * sizeof(Vertex),
                      vertices.data(), &cubeVertices_) ||
        !CreateBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.size() * sizeof(uint16_t),
                      indices.data(), &cubeIndices_) ||
        !CreateBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, lines.size() * sizeof(Vertex),
                      lines.data(), &lineVertices_) ||
        !CreateBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kUniformBufferSize, nullptr,
                      &uniforms_)) {
        return false;
    }

    // Same binding model as the WebGPU backend: one uniform buffer, one dynamic offset per draw.
    VkDescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = 0;
    layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &layoutBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_));

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
    VkDescriptorPoolCreateInfo descriptorPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptorPoolInfo.maxSets = 1;
    descriptorPoolInfo.poolSizeCount = 1;
    descriptorPoolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device_, &descriptorPoolInfo, nullptr, &descriptorPool_));
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_));
    VkDescriptorBufferInfo bufferInfo{uniforms_.buffer, 0, sizeof(Uniforms)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_));
    return true;
}

VkShaderModule VulkanRenderer::CreateShaderModule(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
        LOGE("Vulkan: vkCreateShaderModule failed");
    }
    return module;
}

bool VulkanRenderer::CreateRenderPassAndPipelines(VkFormat colorFormat, VkFormat depthFormat) {
    if (renderPass_ != VK_NULL_HANDLE && colorFormat == pipelineColorFormat_ &&
        depthFormat == pipelineDepthFormat_) {
        return true;
    }
    DestroyRenderPassAndPipelines();

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // OpenXR expects released color images in COLOR_ATTACHMENT_OPTIMAL.
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = depthFormat;
    // The depth is stored: the compositor reads it through XR_KHR_composition_layer_depth.
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_));

    VkShaderModule vert = CreateShaderModule(kSceneVert, sizeof(kSceneVert));
    VkShaderModule lit = CreateShaderModule(kLitFrag, sizeof(kLitFrag));
    VkShaderModule unlit = CreateShaderModule(kUnlitFrag, sizeof(kUnlitFrag));

    VkVertexInputBindingDescription vertexBinding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        attributes[i] = {i, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<uint32_t>(i * 3 * sizeof(float))};
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // The cube is closed and depth-tested, so culling is not needed; skipping it avoids depending
    // on how the flipped viewport affects winding.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthState.depthTestEnable = VK_TRUE;
    depthState.depthWriteEnable = VK_TRUE;
    depthState.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    auto create = [&](VkShaderModule fragment, VkPrimitiveTopology topology, VkPipeline* out) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = topology;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthState;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout_;
        info.renderPass = renderPass_;
        return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, out);
    };
    const bool ok = vert != VK_NULL_HANDLE && lit != VK_NULL_HANDLE && unlit != VK_NULL_HANDLE &&
                    create(lit, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &litPipeline_) == VK_SUCCESS &&
                    create(unlit, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, &linePipeline_) == VK_SUCCESS;
    for (VkShaderModule module : {vert, lit, unlit}) {
        if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, module, nullptr);
    }
    if (!ok) {
        LOGE("Vulkan: pipeline creation failed");
        return false;
    }
    pipelineColorFormat_ = colorFormat;
    pipelineDepthFormat_ = depthFormat;
    return true;
}

void VulkanRenderer::DestroyRenderPassAndPipelines() {
    if (litPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, litPipeline_, nullptr);
    if (linePipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, linePipeline_, nullptr);
    if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);
    litPipeline_ = linePipeline_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    pipelineColorFormat_ = pipelineDepthFormat_ = VK_FORMAT_UNDEFINED;
}

bool VulkanRenderer::CreateSwapchain(XrSession session, const std::vector<int64_t>& formats,
                                     const std::vector<VkFormat>& preferred,
                                     XrSwapchainUsageFlags usage, VkImageAspectFlags aspect,
                                     uint32_t width, uint32_t height, SwapchainImages* out) {
    for (VkFormat candidate : preferred) {
        if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
            out->format = candidate;
            break;
        }
    }
    if (out->format == VK_FORMAT_UNDEFINED) {
        LOGE("Vulkan: runtime offers none of the preferred swapchain formats");
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = usage;
    info.format = out->format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(session, &info, &out->swapchain));

    uint32_t count = 0;
    XR_CHECK(xrEnumerateSwapchainImages(out->swapchain, 0, &count, nullptr));
    std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(out->swapchain, count, &count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    for (const auto& image : images) {
        out->images.push_back(image.image);
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = out->format;
        viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &view));
        out->views.push_back(view);
    }
    return true;
}

void VulkanRenderer::DestroySwapchain(SwapchainImages* swapchain) {
    for (VkImageView view : swapchain->views) vkDestroyImageView(device_, view, nullptr);
    if (swapchain->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(swapchain->swapchain);
    *swapchain = {};
}

bool VulkanRenderer::EnsureSwapchains(XrSession session, uint32_t eyeWidth, uint32_t eyeHeight) {
    if (color_.swapchain != XR_NULL_HANDLE && eyeWidth == eyeWidth_ && eyeHeight == eyeHeight_) {
        return true;
    }
    DestroySwapchains();
    if (swapchainFailed_) return false;
    swapchainFailed_ = true;  // Cleared on success, so a failure is not retried every frame.

    uint32_t count = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    XR_CHECK(xrEnumerateSwapchainFormats(session, count, &count, formats.data()));

    // UNORM first, to match the colors of the WebGPU surface path.
    const uint32_t width = eyeWidth * 2;  // Both eyes side by side, as in the WebGPU backend.
    if (!CreateSwapchain(session, formats,
                         {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB},
                         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, width,
                         eyeHeight, &color_) ||
        !CreateSwapchain(session, formats,
                         {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                          VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM},
                         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                         width, eyeHeight, &depth_) ||
        !CreateRenderPassAndPipelines(color_.format, depth_.format)) {
        DestroySwapchains();
        return false;
    }

    for (VkImageView colorView : color_.views) {
        for (VkImageView depthView : depth_.views) {
            const VkImageView attachments[] = {colorView, depthView};
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = renderPass_;
            info.attachmentCount = 2;
            info.pAttachments = attachments;
            info.width = width;
            info.height = eyeHeight;
            info.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFramebuffer(device_, &info, nullptr, &framebuffer));
            framebuffers_.push_back(framebuffer);
        }
    }

    eyeWidth_ = eyeWidth;
    eyeHeight_ = eyeHeight;
    swapchainFailed_ = false;
    LOGI("Vulkan: swapchains %ux%u per eye, color format %d (%zu images), depth format %d "
         "(%zu images)",
         eyeWidth, eyeHeight, color_.format, color_.images.size(), depth_.format,
         depth_.images.size());
    return true;
}

void VulkanRenderer::DestroySwapchains() {
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);
    for (VkFramebuffer framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();
    DestroySwapchain(&color_);
    DestroySwapchain(&depth_);
    eyeWidth_ = eyeHeight_ = 0;
}

bool VulkanRenderer::Render(const EyeView (&eyes)[2], const SceneState& scene,
                            XrCompositionLayerProjectionView (&views)[2]) {
    if (color_.swapchain == XR_NULL_HANDLE) return false;

    // The previous frame must be done before its uniforms and command buffer are reused.
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);

    uint32_t colorIndex = 0, depthIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrAcquireSwapchainImage(color_.swapchain, &acquireInfo, &colorIndex));
    XR_CHECK(xrWaitSwapchainImage(color_.swapchain, &waitInfo));
    XR_CHECK(xrAcquireSwapchainImage(depth_.swapchain, &acquireInfo, &depthIndex));
    XR_CHECK(xrWaitSwapchainImage(depth_.swapchain, &waitInfo));

    PackUniforms(eyes, scene, static_cast<uint8_t*>(uniforms_.mapped));

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &beginInfo);

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // Transparent around the content.
    clears[1].depthStencil = {1.0f, 0};            // Far plane where nothing is drawn.
    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = renderPass_;
    passInfo.framebuffer = framebuffers_[colorIndex * depth_.views.size() + depthIndex];
    passInfo.renderArea = {{0, 0}, {eyeWidth_ * 2, eyeHeight_}};
    passInfo.clearValueCount = 2;
    passInfo.pClearValues = clears;
    vkCmdBeginRenderPass(commandBuffer_, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    const VkDeviceSize zero = 0;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const EyeView& view = eyes[eye];
        // Negative height flips Y so the shared WebGPU-convention projection (+Y up) is used as is.
        VkViewport viewport{static_cast<float>(view.viewportX),
                            static_cast<float>(view.viewportY + view.viewportHeight),
                            static_cast<float>(view.viewportWidth),
                            -static_cast<float>(view.viewportHeight), 0.0f, 1.0f};
        VkRect2D scissor{{static_cast<int32_t>(view.viewportX), static_cast<int32_t>(view.viewportY)},
                         {view.viewportWidth, view.viewportHeight}};
        vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);

        auto bind = [&](DrawSlot slot) {
            const uint32_t offset = UniformOffset(eye, slot);
            vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 0, 1, &descriptorSet_, 1, &offset);
        };
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, litPipeline_);
        vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &cubeVertices_.buffer, &zero);
        vkCmdBindIndexBuffer(commandBuffer_, cubeIndices_.buffer, 0, VK_INDEX_TYPE_UINT16);
        bind(kSlotCube);
        vkCmdDrawIndexed(commandBuffer_, cubeIndexCount_, 1, 0, 0, 0);
        bind(kSlotToggle);
        vkCmdDrawIndexed(commandBuffer_, cubeIndexCount_, 1, 0, 0, 0);

        if (scene.showBounds) {
            vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
            vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &lineVertices_.buffer, &zero);
            bind(kSlotBounds);
            vkCmdDraw(commandBuffer_, lineVertexCount_, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(commandBuffer_);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    // Same queue as the binding: the runtime's reads are ordered after this submission.
    VK_CHECK(vkQueueSubmit(queue_, 1, &submitInfo, fence_));

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(color_.swapchain, &releaseInfo));
    XR_CHECK(xrReleaseSwapchainImage(depth_.swapchain, &releaseInfo));

    for (uint32_t eye = 0; eye < 2; ++eye) {
        const XrRect2Di rect{{static_cast<int32_t>(eyes[eye].viewportX), 0},
                             {static_cast<int32_t>(eyes[eye].viewportWidth),
                              static_cast<int32_t>(eyes[eye].viewportHeight)}};
        views[eye].subImage.swapchain = color_.swapchain;
        views[eye].subImage.imageRect = rect;
        views[eye].subImage.imageArrayIndex = 0;
        if (layerDepthEnabled_) {
            XrCompositionLayerDepthInfoKHR& depth = depthInfos_[eye];
            depth = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            depth.subImage.swapchain = depth_.swapchain;
            depth.subImage.imageRect = rect;
            depth.subImage.imageArrayIndex = 0;
            depth.minDepth = 0.0f;
            depth.maxDepth = 1.0f;
            depth.nearZ = kNearZ;
            depth.farZ = kFarZ;
            views[eye].next = &depth;
        }
    }
    if (layerDepthEnabled_ && !loggedDepth_) {
        LOGI("Vulkan: submitting layer depth (near %.2f, far %.0f)", kNearZ, kFarZ);
        loggedDepth_ = true;
    }
    return true;
}

}  // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer() { return std::make_unique<VulkanRenderer>(); }
