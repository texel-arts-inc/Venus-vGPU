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

bool intersectRoom(vec3 origin, vec3 direction, out Vertex vertex, out float distance) {
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

bool roomOccludes(vec3 origin, vec3 direction, float distance) {
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

#include "spectral-table.glsl"

// The Vulkan acceleration structure holds the box. Analytic sphere hits
// give the glass a smooth, closed interface with an exact surface normal.
layout(constant_id = 0) const bool showSpheres = false;

const vec4 spheres[3] = vec4[3](
    vec4(-.365, .230, -.06, .230),
    vec4( .370, .226, -.09, .226),
    vec4( .000, .226,  .53, .226)
);

float sphereDistance(vec3 origin, vec3 direction, vec4 sphere) {
    vec3 relative = origin - sphere.xyz;
    float projected = dot(relative, direction);
    float discriminant = projected * projected -
                         (dot(relative, relative) - sphere.w * sphere.w);
    if (discriminant < 0.0)
        return 30.0;
    float root = sqrt(discriminant);
    float nearHit = -projected - root;
    float farHit = -projected + root;
    return nearHit > .0003 ? nearHit : (farHit > .0003 ? farHit : 30.0);
}

bool occluded(vec3 origin, vec3 direction, float distance) {
    for (int i = 0; showSpheres && i < 3; ++i) {
        if (sphereDistance(origin, direction, spheres[i]) < distance - .001)
            return true;
    }
    return roomOccludes(origin, direction, distance);
}

float dielectricReflectance(float cosineIn, float cosineOut, float eta) {
    float s = (eta * cosineIn - cosineOut) / (eta * cosineIn + cosineOut);
    float p = (cosineIn - eta * cosineOut) / (cosineIn + eta * cosineOut);
    return .5 * (s * s + p * p);
}

vec3 samplePath(vec3 direction, inout uint state) {
    vec3 radiance = vec3(0);
    vec3 throughput = vec3(1);
    uvec3 paintBounces = uvec3(0);
    uint diffuseBounces = 0;
    bool previousSpecular = true;
    vec3 origin = camera;
    for (uint bounce = 0; bounce < 12; ++bounce) {
        Vertex hit;
        float distance = 30.0;
        bool roomHit = intersectRoom(origin, direction, hit, distance);
        if (!roomHit)
            distance = 30.0;
        int sphere = -1;
        for (int i = 0; showSpheres && i < 3; ++i) {
            float t = sphereDistance(origin, direction, spheres[i]);
            if (t < distance) {
                distance = t;
                sphere = i;
            }
        }
        if (!roomHit && sphere < 0) {
            if (bounce == 0)
                radiance += vec3(.006, .008, .010);
            break;
        }

        uint paintIndex = 36u * paintBounces.x + 6u * paintBounces.y + paintBounces.z;
        if (sphere < 0 && hit.material.a != 0.0) {
            if (previousSpecular)
                radiance += throughput * spectralRadiance[paintIndex];
            break;
        }

        vec3 point = origin + direction * distance;
        vec3 normal;
        if (sphere < 0)
            normal = hit.normal.xyz;
        else
            normal = normalize(point - spheres[sphere].xyz);
        if (sphere == 2) {
            bool entering = dot(direction, normal) < 0.0;
            vec3 facing = entering ? normal : -normal;
            const float glassIor = 1.50;
            float eta = entering ? 1.0 / glassIor : glassIor;
            float cosineIn = clamp(-dot(direction, facing), 0.0, 1.0);
            float cosineOutSquared = 1.0 - eta * eta * (1.0 - cosineIn * cosineIn);
            if (cosineOutSquared <= 0.0 ||
                randomFloat(state) < dielectricReflectance(cosineIn, sqrt(cosineOutSquared), eta)) {
                direction = reflect(direction, facing);
                origin = point + facing * .0006;
            } else {
                direction = refract(direction, facing, eta);
                origin = point - facing * .0006;
                throughput *= eta * eta;
            }
            previousSpecular = true;
            continue;
        }

        origin = point + normal * .0006;
        if (sphere == 1) {
            float cosine = clamp(-dot(direction, normal), 0.0, 1.0);
            float glazeFresnel = .046 + .954 * pow(1.0 - cosine, 5.0);
            if (randomFloat(state) < glazeFresnel) {
                direction = reflect(direction, normal);
                previousSpecular = true;
                continue;
            }
        }

        if (diffuseBounces == 5)
            break;
        ++diffuseBounces;
        if (sphere == 0) {
            throughput *= vec3(.115, .087, .041);
        } else if (sphere == 1) {
            throughput *= vec3(.248, .028, .035);
        } else {
            vec3 material = hit.material.rgb;
            if (material.r > 2.0 * material.g)
                ++paintBounces.z;
            else if (material.g > 2.0 * material.r)
                ++paintBounces.y;
            else
                ++paintBounces.x;
        }

        vec2 offset = (randomPair(state) - .5) * lightSize;
        vec3 toLight = lightCenter + vec3(offset.x, 0, offset.y) - origin;
        float lightDistance = length(toLight);
        vec3 toLightDirection = toLight / lightDistance;
        float geometry = max(0.0, dot(normal, toLightDirection)) * max(0.0, toLightDirection.y);
        if (geometry > 0.0 && !occluded(origin, toLightDirection, lightDistance)) {
            paintIndex = 36u * paintBounces.x + 6u * paintBounces.y + paintBounces.z;
            radiance += throughput * spectralRadiance[paintIndex] * geometry * lightSize.x * lightSize.y /
                        (pi * lightDistance * lightDistance);
        }
        direction = cosineHemisphere(normal, randomPair(state));
        previousSpecular = false;
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
    outColor = vec4(displayColor(max(color / float(settings.samples), vec3(0))), 1);
}
