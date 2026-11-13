// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#version 460
#extension GL_GOOGLE_include_directive : require
#include "shared.glsl"

layout(location = 0) in vec3 worldPosition;
layout(location = 1) flat in vec3 normal;
layout(location = 2) flat in vec4 material;
layout(location = 0) out vec4 outColor;

float blockContact(vec2 p, vec2 center, vec2 halfSize, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    vec2 local = mat2(c, s, -s, c) * (p - center);
    float distanceToBlock = length(max(abs(local) - halfSize, vec2(0)));
    return smoothstep(.006, .31, distanceToBlock);
}

void main() {
    if (material.a != 0) {
        outColor = vec4(displayColor(lightRadiance), 1);
        return;
    }

    vec3 towardLight = lightCenter - worldPosition;
    float distanceSquared = dot(towardLight, towardLight);
    vec3 lightDirection = towardLight * inversesqrt(max(.01, distanceSquared));
    float direct = max(0, dot(normal, lightDirection)) * max(0, lightDirection.y) *
                   lightSize.x * lightSize.y / (pi * max(.15, distanceSquared));

    // Diffuse fill and contact shading use the shape of the room and its two plinths.
    vec3 wallBounce = vec3(.16, .10, .075);
    wallBounce += vec3(.18, .020, .012) * exp(-3.0 * (worldPosition.x + 1.0));
    wallBounce += vec3(.014, .17, .028) * exp(-3.0 * (1.0 - worldPosition.x));
    float contact = 1;
    if (normal.y > .95 && worldPosition.y < .001) {
        contact = .30 + .70 * blockContact(worldPosition.xz, vec2(-.40, -.36), vec2(.285), -.29) *
                               blockContact(worldPosition.xz, vec2(.38, .28), vec2(.32, .31), .31);
    }
    float heightFill = .55 + .45 * smoothstep(0.0, 1.6, worldPosition.y);
    vec3 lighting = (wallBounce * heightFill + lightRadiance * direct) * contact;
    outColor = vec4(displayColor(material.rgb * lighting), 1);
}
