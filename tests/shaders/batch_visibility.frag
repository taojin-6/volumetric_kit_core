#version 450

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

layout(set = 0, binding = 2) uniform FragmentUniform { uint value; } fragment_uniform;
layout(std430, set = 0, binding = 3) readonly buffer FragmentStorage {
  uint value;
} fragment_storage;
layout(location = 0) flat in uvec2 vertex_values;
layout(location = 0) out vec4 color;

void main() {
  color = vec4(vertex_values, fragment_uniform.value, fragment_storage.value) / 255.0;
}
