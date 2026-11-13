// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

layout(push_constant) uniform Settings {
    vec2 extent;
    uint samples;
    uint encodeSrgb;
} settings;

const vec3 camera = vec3(0, 1.00, 3.72);
const float halfFov = .391;
const vec3 lightRadiance = vec3(20.0, 18.0, 15.5);
const vec3 lightCenter = vec3(0, 1.993, -.04);
const vec2 lightSize = vec2(.60, .56);
const float pi = 3.14159265359;

struct Vertex {
    vec4 position;
    vec4 normal;
    vec4 material;
};

vec4 project(vec3 position) {
    vec3 view = position - camera;
    float depth = -view.z;
    float aspect = settings.extent.x / settings.extent.y;
    const float near = .05;
    const float far = 30.0;
    return vec4(view.x / (halfFov * aspect), -view.y / halfFov,
                (depth - near) * (far / (far - near)), depth);
}

vec3 displayColor(vec3 color) {
    // Filmic tonemapping, then an optional transfer function for UNORM swapchains.
    color = clamp((color * (2.51 * color + .03)) /
                  (color * (2.43 * color + .59) + .14), 0.0, 1.0);
    if (settings.encodeSrgb != 0) {
        color = mix(1.055 * pow(color, vec3(1.0 / 2.4)) - .055, 12.92 * color,
                    lessThanEqual(color, vec3(.0031308)));
    }
    return color;
}
