// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#version 460
#extension GL_GOOGLE_include_directive : require
#include "shared.glsl"

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inMaterial;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) flat out vec3 normal;
layout(location = 2) flat out vec4 material;

void main() {
    gl_Position = project(inPosition.xyz);
    worldPosition = inPosition.xyz;
    normal = inNormal.xyz;
    material = inMaterial;
}
