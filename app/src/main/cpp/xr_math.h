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

// Minimal column-major 4x4 matrix helpers for the demo (WebGPU clip space: z in [0, 1]).

#pragma once

#include <cmath>

#include <openxr/openxr.h>

struct Mat4 {
    float m[16];  // Column-major.

    static Mat4 Identity() {
        return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    }

    static Mat4 Scale(float s) {
        return {{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1}};
    }

    static Mat4 Scale(float x, float y, float z) {
        return {{x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}};
    }

    static Mat4 RotationY(float radians) {
        const float c = std::cos(radians), s = std::sin(radians);
        return {{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}};
    }

    static Mat4 RotationX(float radians) {
        const float c = std::cos(radians), s = std::sin(radians);
        return {{1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1}};
    }

    // Rigid transform from an OpenXR pose.
    static Mat4 FromPose(const XrPosef& pose) {
        const float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z,
                    w = pose.orientation.w;
        Mat4 r{};
        r.m[0] = 1 - 2 * (y * y + z * z);
        r.m[1] = 2 * (x * y + z * w);
        r.m[2] = 2 * (x * z - y * w);
        r.m[4] = 2 * (x * y - z * w);
        r.m[5] = 1 - 2 * (x * x + z * z);
        r.m[6] = 2 * (y * z + x * w);
        r.m[8] = 2 * (x * z + y * w);
        r.m[9] = 2 * (y * z - x * w);
        r.m[10] = 1 - 2 * (x * x + y * y);
        r.m[12] = pose.position.x;
        r.m[13] = pose.position.y;
        r.m[14] = pose.position.z;
        r.m[15] = 1;
        return r;
    }

    // Inverse of a rigid transform (rotation + translation).
    Mat4 RigidInverse() const {
        Mat4 r{};
        for (int c = 0; c < 3; ++c) {
            for (int row = 0; row < 3; ++row) r.m[c * 4 + row] = m[row * 4 + c];
        }
        for (int row = 0; row < 3; ++row) {
            r.m[12 + row] = -(r.m[row] * m[12] + r.m[4 + row] * m[13] + r.m[8 + row] * m[14]);
        }
        r.m[15] = 1;
        return r;
    }

    // Asymmetric-FOV perspective projection mapping depth to [0, 1].
    static Mat4 Projection(const XrFovf& fov, float nearZ, float farZ) {
        const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
        const float d = std::tan(fov.angleDown), u = std::tan(fov.angleUp);
        const float w = r - l, h = u - d;
        Mat4 p{};
        p.m[0] = 2 / w;
        p.m[5] = 2 / h;
        p.m[8] = (r + l) / w;
        p.m[9] = (u + d) / h;
        p.m[10] = -farZ / (farZ - nearZ);
        p.m[11] = -1;
        p.m[14] = -(farZ * nearZ) / (farZ - nearZ);
        return p;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = sum;
            }
        }
        return r;
    }
};
