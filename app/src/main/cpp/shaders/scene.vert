#version 450
// Vulkan backend vertex shader; mirrors vs_main in webgpu_renderer.cpp.
// The uniform block matches struct Uniforms in scene.h (std140, column-major).

layout(set = 0, binding = 0) uniform Uniforms {
    mat4 mvp;
    mat4 model;
    vec4 tint;
} u;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;
layout(location = 2) out vec3 outObjectNormal;

void main() {
    gl_Position = u.mvp * vec4(inPosition, 1.0);
    outNormal = (u.model * vec4(inNormal, 0.0)).xyz;
    outColor = inColor;
    outObjectNormal = inNormal;
}
