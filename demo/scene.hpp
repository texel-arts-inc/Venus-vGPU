// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <cmath>
#include <vector>

struct Vec3 {
    float x, y, z;
};

inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 normalized(Vec3 a) {
    const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return {a.x / length, a.y / length, a.z / length};
}

// This also matches the shaders' std430 array stride and the BLAS vertex stride.
struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> normal;
    std::array<float, 4> material;
};

inline std::vector<Vertex> cornellBox(bool includeBlocks) {
    std::vector<Vertex> vertices;
    constexpr Vec3 white{0.76f, 0.73f, 0.68f};
    constexpr Vec3 red{0.72f, 0.045f, 0.030f};
    constexpr Vec3 green{0.067f, 0.49f, 0.106f};
    constexpr Vec3 plaster{0.79f, 0.76f, 0.71f};

    const auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 color, float emission = 0) {
        const Vec3 normal = normalized(cross(b - a, c - a));
        for (const auto position : {a, b, c, a, c, d}) {
            vertices.push_back({{position.x, position.y, position.z, 1},
                                {normal.x, normal.y, normal.z, 0},
                                {color.x, color.y, color.z, emission}});
        }
    };

    quad({-1, 0, 1}, {1, 0, 1}, {1, 0, -1}, {-1, 0, -1}, white);
    quad({-1, 2, -1}, {1, 2, -1}, {1, 2, 1}, {-1, 2, 1}, white);
    quad({-1, 0, -1}, {1, 0, -1}, {1, 2, -1}, {-1, 2, -1}, white);
    quad({-1, 0, -1}, {-1, 2, -1}, {-1, 2, 1}, {-1, 0, 1}, red);
    quad({1, 0, 1}, {1, 2, 1}, {1, 2, -1}, {1, 0, -1}, green);
    quad({-.30f, 1.993f, -.32f}, {.30f, 1.993f, -.32f},
         {.30f, 1.993f, .24f}, {-.30f, 1.993f, .24f}, {1, 1, 1}, 1);

    const auto block = [&](float x, float z, float halfX, float halfZ, float height, float angle) {
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const auto point = [&](float px, float py, float pz) {
            return Vec3{x + cosine * px + sine * pz, py, z - sine * px + cosine * pz};
        };
        const Vec3 a = point(-halfX, 0, -halfZ), b = point(halfX, 0, -halfZ);
        const Vec3 c = point(halfX, 0, halfZ), d = point(-halfX, 0, halfZ);
        const Vec3 A{a.x, height, a.z}, B{b.x, height, b.z};
        const Vec3 C{c.x, height, c.z}, D{d.x, height, d.z};
        quad(a, A, B, b, plaster);
        quad(b, B, C, c, plaster);
        quad(c, C, D, d, plaster);
        quad(d, D, A, a, plaster);
        quad(A, D, C, B, plaster);
    };
    if (includeBlocks) {
        block(-.40f, -.36f, .285f, .285f, 1.16f, -.29f);
        block(.38f, .28f, .32f, .31f, .59f, .31f);
    }
    return vertices;
}
