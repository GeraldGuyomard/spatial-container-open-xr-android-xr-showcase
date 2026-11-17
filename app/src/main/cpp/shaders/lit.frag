#version 450
// Vulkan backend lit shader; keep in sync with fs_lit in webgpu_renderer.cpp.

layout(set = 0, binding = 0) uniform Uniforms {
    mat4 mvp;
    mat4 model;
    vec4 tint;
} u;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inObjectNormal;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 light = normalize(vec3(0.4, 0.8, 0.5));
    float diffuse = 0.35 + 0.65 * max(dot(normalize(inNormal), light), 0.0);
    // tint.w selects the palette: 0 = bounded (cool blues), 1 = immersive (warm oranges),
    // 2 = plain white (the toggle cube).
    vec3 f = inObjectNormal * 0.5 + 0.5;
    vec3 cool = vec3(0.10 + 0.15 * f.z, 0.40 + 0.35 * f.x, 0.70 + 0.30 * f.y);
    vec3 warm = vec3(0.95, 0.30 + 0.40 * f.x, 0.05 + 0.20 * f.y + 0.10 * f.z);
    vec3 base = u.tint.w > 1.5 ? vec3(0.95) : (u.tint.w > 0.5 ? warm : cool);
    outColor = vec4(base * diffuse * u.tint.rgb, 1.0);
}
