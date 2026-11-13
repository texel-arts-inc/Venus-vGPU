// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#version 460

void main() {
    const vec2 corners[] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(corners[gl_VertexIndex], 0, 1);
}
