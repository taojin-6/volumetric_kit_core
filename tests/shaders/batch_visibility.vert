#version 450

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

layout(set = 0, binding = 0) uniform VertexUniform { uint value; } vertex_uniform;
layout(std430, set = 0, binding = 1) readonly buffer VertexStorage {
  uint value;
} vertex_storage;
layout(location = 0) flat out uvec2 vertex_values;

void main() {
  vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
  vertex_values = uvec2(vertex_uniform.value, vertex_storage.value);
}
