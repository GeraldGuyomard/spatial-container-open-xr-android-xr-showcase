#version 450
// Vulkan backend unlit shader (bounds wireframe); mirrors fs_unlit in webgpu_renderer.cpp.

layout(set = 0, binding = 0) uniform Uniforms {
    mat4 mvp;
    mat4 model;
    vec4 tint;
} u;

layout(location = 1) in vec3 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(inColor * u.tint.rgb, 1.0);
}
