// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "shared.glsl"

layout(std430, set = 0, binding = 0) readonly buffer Scene {
    Vertex vertices[];
};
layout(set = 0, binding = 1) uniform accelerationStructureEXT scene;
layout(location = 0) out vec4 outColor;

uint randomBits(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint bits = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (bits >> 22u) ^ bits;
}

float randomFloat(inout uint state) {
    return float(randomBits(state) >> 8u) * (1.0 / 16777216.0);
}

vec2 randomPair(inout uint state) {
    float x = randomFloat(state);
    return vec2(x, randomFloat(state));
}

bool intersectScene(vec3 origin, vec3 direction, out Vertex vertex, out float distance) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, 0xff, origin, .0003, direction, 30.0);
    while (rayQueryProceedEXT(query)) {}
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        return false;
    }
    vertex = vertices[3 * rayQueryGetIntersectionPrimitiveIndexEXT(query, true)];
    distance = rayQueryGetIntersectionTEXT(query, true);
    return true;
}

bool occluded(vec3 origin, vec3 direction, float distance) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          0xff, origin, .0003, direction, distance - .001);
    while (rayQueryProceedEXT(query)) {}
    return rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec3 cosineHemisphere(vec3 normal, vec2 u) {
    vec3 tangent = normalize(cross(abs(normal.y) > .99 ? vec3(1, 0, 0) : vec3(0, 1, 0), normal));
    vec3 bitangent = cross(normal, tangent);
    float angle = 2.0 * pi * u.y;
    return sqrt(u.x) * (tangent * cos(angle) + bitangent * sin(angle)) +
           sqrt(1.0 - u.x) * normal;
}

vec3 samplePath(vec3 direction, inout uint state) {
    vec3 radiance = vec3(0);
    vec3 throughput = vec3(1);
    vec3 origin = camera;
    for (uint bounce = 0; bounce < 5; ++bounce) {
        Vertex hit;
        float distance;
        if (!intersectScene(origin, direction, hit, distance)) {
            if (bounce == 0) {
                radiance += vec3(.006, .008, .010);
            }
            break;
        }
        if (hit.material.a != 0) {
            if (bounce == 0) {
                radiance += lightRadiance;
            }
            break;
        }

        vec3 normal = hit.normal.xyz;
        origin += direction * distance + normal * .0006;
        throughput *= hit.material.rgb;

        vec2 offset = (randomPair(state) - .5) * lightSize;
        vec3 toLight = lightCenter + vec3(offset.x, 0, offset.y) - origin;
        float lightDistance = length(toLight);
        vec3 toLightDirection = toLight / lightDistance;
        float geometry = max(0, dot(normal, toLightDirection)) * max(0, toLightDirection.y);
        if (geometry > 0 && !occluded(origin, toLightDirection, lightDistance)) {
            radiance += throughput * lightRadiance * geometry * lightSize.x * lightSize.y /
                        (pi * lightDistance * lightDistance);
        }
        direction = cosineHemisphere(normal, randomPair(state));
    }
    return radiance;
}

void main() {
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint state = pixel.x * 1973u + pixel.y * 9277u + 89173u;
    vec3 color = vec3(0);
    for (uint sampleIndex = 0; sampleIndex < settings.samples; ++sampleIndex) {
        vec2 jitter = randomPair(state) - .5;
        vec2 ndc = (gl_FragCoord.xy + jitter) / settings.extent * 2.0 - 1.0;
        ndc.x *= settings.extent.x / settings.extent.y;
        vec3 direction = normalize(vec3(ndc.x * halfFov, -ndc.y * halfFov, -1));
        color += samplePath(direction, state);
    }
    outColor = vec4(displayColor(color / float(settings.samples)), 1);
}
