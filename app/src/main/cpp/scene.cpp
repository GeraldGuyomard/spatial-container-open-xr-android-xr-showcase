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

#include "scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

void BuildCube(std::vector<Vertex>* vertices, std::vector<uint16_t>* indices) {
    struct Face {
        float normal[3], u[3], v[3], color[3];
    };
    const Face faces[] = {
            {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0.92f, 0.30f, 0.25f}},
            {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0.25f, 0.75f, 0.35f}},
            {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {0.24f, 0.86f, 0.52f}},
            {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0.95f, 0.75f, 0.20f}},
            {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0.26f, 0.52f, 0.96f}},
            {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {0.70f, 0.40f, 0.90f}},
    };
    for (const Face& f : faces) {
        const auto base = static_cast<uint16_t>(vertices->size());
        constexpr std::pair<int, int> kCorners[] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& [su, sv] : kCorners) {
            Vertex vertex{};
            for (int i = 0; i < 3; ++i) {
                vertex.position[i] = f.normal[i] + su * f.u[i] + sv * f.v[i];
                vertex.normal[i] = f.normal[i];
                vertex.color[i] = f.color[i];
            }
            vertices->push_back(vertex);
        }
        for (uint16_t i : {0, 1, 2, 0, 2, 3}) indices->push_back(base + i);
    }
}

std::vector<Vertex> BuildBoxEdges() {
    std::vector<Vertex> lines;
    const float color[3] = {0.3f, 0.9f, 1.0f};
    auto corner = [](int i) {
        return std::array<float, 3>{i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : -1.f};
    };
    for (int a = 0; a < 8; ++a) {
        for (int bit : {1, 2, 4}) {
            const int b = a | bit;
            if (b == a) continue;
            for (int end : {a, b}) {
                const auto p = corner(end);
                lines.push_back({{p[0], p[1], p[2]}, {0, 0, 0}, {color[0], color[1], color[2]}});
            }
        }
    }
    return lines;
}

void BuildEyeViews(const XrView (&views)[2], uint32_t eyeWidth, uint32_t eyeHeight,
                   EyeView (&eyes)[2]) {
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const Mat4 view = Mat4::FromPose(views[eye].pose).RigidInverse();
        eyes[eye].viewProjection = Mat4::Projection(views[eye].fov, kNearZ, kFarZ) * view;
        eyes[eye].viewportX = eye * eyeWidth;
        eyes[eye].viewportY = 0;
        eyes[eye].viewportWidth = eyeWidth;
        eyes[eye].viewportHeight = eyeHeight;
    }
}

SceneState BuildScene(const SceneInput& in) {
    // Fit the cube inside the container in bounded mode; fixed real-world size when immersive.
    const float fit = in.infiniteBounds
                              ? 0.3f
                              : 0.4f * std::min({in.bounds.width, in.bounds.height, in.bounds.depth});
    const float pulse = in.immersive ? 1.0f + 0.05f * std::sin(in.timeSeconds * 3.1416f) : 1.0f;

    SceneState scene{};
    scene.cubeModel = Mat4::RotationY(in.timeSeconds * 0.8f) *
                      Mat4::RotationX(in.timeSeconds * 0.5f) * Mat4::Scale(fit * pulse);
    scene.immersive = in.immersive;
    scene.boundsHalf[0] = in.bounds.width * 0.5f;
    scene.boundsHalf[1] = in.bounds.height * 0.5f;
    scene.boundsHalf[2] = in.bounds.depth * 0.5f;
    scene.showBounds = !in.immersive && !in.infiniteBounds;
    scene.highlight = in.interactable;

    // Not rotating; slightly enlarged while a hand aims at it.
    const float toggleHalf = in.toggleHalfExtent * (in.toggleHovered ? 1.2f : 1.0f);
    scene.toggleCubeModel = Mat4::Scale(toggleHalf);
    scene.toggleCubeModel.m[12] = in.toggleCenter.x;
    scene.toggleCubeModel.m[13] = in.toggleCenter.y;
    scene.toggleCubeModel.m[14] = in.toggleCenter.z;
    return scene;
}

void PackUniforms(const EyeView (&eyes)[2], const SceneState& scene, uint8_t* out) {
    std::memset(out, 0, kUniformBufferSize);
    const float brightness = scene.highlight ? 1.25f : 1.0f;
    const float sceneTint[4] = {brightness, brightness, brightness, scene.immersive ? 1.0f : 0.0f};
    const float white[4] = {1.0f, 1.0f, 1.0f, 2.0f};
    const Mat4 boundsModel =
            Mat4::Scale(scene.boundsHalf[0], scene.boundsHalf[1], scene.boundsHalf[2]);

    auto write = [&](uint32_t eye, DrawSlot slot, const Mat4& model, const float (&tint)[4]) {
        Uniforms u{};
        std::memcpy(u.mvp, (eyes[eye].viewProjection * model).m, sizeof(u.mvp));
        std::memcpy(u.model, model.m, sizeof(u.model));
        std::memcpy(u.tint, tint, sizeof(u.tint));
        std::memcpy(out + UniformOffset(eye, slot), &u, sizeof(u));
    };
    for (uint32_t eye = 0; eye < 2; ++eye) {
        write(eye, kSlotCube, scene.cubeModel, sceneTint);
        write(eye, kSlotBounds, boundsModel, sceneTint);
        write(eye, kSlotToggle, scene.toggleCubeModel, white);
    }
}
