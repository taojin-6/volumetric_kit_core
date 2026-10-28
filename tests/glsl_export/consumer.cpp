// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Compiles against the header vkc_embed_shaders generates from scaled.comp,
// whose kernel includes the exported GLSL: the build gets here only once that
// include resolved through INCLUDE_TARGETS.

#include <cstddef>

#include "scaled_comp.spv.hpp"

std::size_t vkc_glsl_scaled_size() { return vkc_glsl_scaled_comp_spv_size; }
