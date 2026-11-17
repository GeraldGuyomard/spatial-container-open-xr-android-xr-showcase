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

// Backend-independent scene: geometry, per-frame animation, eye matrices and the per-draw uniform
// bytes. Every renderer uploads the same data and draws the same slots, so the backends only differ
// in how they talk to the GPU.

#include <openxr/openxr.h>

#include <cstdint>
#include <vector>

#include "xr_math.h"

// Clip planes shared by the projection matrices and the layer depth info.
constexpr float kNearZ = 0.05f;
constexpr float kFarZ = 100.0f;

struct Vertex {
    float position[3];
    float normal[3];
    float color[3];
};

// Unit cube (positions in [-1, 1]) with per-face normals, as an indexed triangle list.
void BuildCube(std::vector<Vertex>* vertices, std::vector<uint16_t>* indices);
// The 12 edges of the unit cube as a line list (the container bounds wireframe).
std::vector<Vertex> BuildBoxEdges();

struct EyeView {
    // Projection uses [0, 1] clip depth and +Y up in clip space (WebGPU conventions).
    Mat4 viewProjection;
    uint32_t viewportX, viewportY, viewportWidth, viewportHeight;
};

// Both eyes side by side in one image of (2 * eyeWidth) x eyeHeight.
void BuildEyeViews(const XrView (&views)[2], uint32_t eyeWidth, uint32_t eyeHeight,
                   EyeView (&eyes)[2]);

// What the app knows this frame; BuildScene turns it into transforms.
struct SceneInput {
    float timeSeconds;
    XrExtent3Df bounds;
    bool infiniteBounds;
    bool immersive;      // Runtime-confirmed bounds mode.
    bool interactable;   // Container has input focus.
    XrVector3f toggleCenter;
    float toggleHalfExtent;
    bool toggleHovered;
};

struct SceneState {
    Mat4 cubeModel;
    // The white, non-rotating cube that toggles the bounds mode when picked.
    Mat4 toggleCubeModel;
    // Half extents of the container bounds; the wireframe is drawn only when showBounds is set.
    float boundsHalf[3];
    bool showBounds;
    bool highlight;
    // Selects the cube palette: cool blues when bounded, warm oranges when immersive.
    bool immersive;
};

SceneState BuildScene(const SceneInput& input);

// Per-draw uniforms, one slot per draw per eye at a 256-byte stride (dynamic uniform offsets).
struct Uniforms {
    float mvp[16];
    float model[16];
    // rgb multiplies the color; w selects the palette: 0 cool, 1 warm, 2 white.
    float tint[4];
};
constexpr uint32_t kUniformStride = 256;
enum DrawSlot : uint32_t { kSlotCube = 0, kSlotBounds = 1, kSlotToggle = 2, kSlotsPerEye = 3 };
constexpr uint32_t kUniformBufferSize = kUniformStride * kSlotsPerEye * 2;

inline uint32_t UniformOffset(uint32_t eye, DrawSlot slot) {
    return (eye * kSlotsPerEye + slot) * kUniformStride;
}

// Fills `out` (kUniformBufferSize bytes) with the uniforms of every draw for both eyes.
void PackUniforms(const EyeView (&eyes)[2], const SceneState& scene, uint8_t* out);
