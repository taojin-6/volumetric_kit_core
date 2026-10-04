// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: reduces a resource's queue-family list to its
// distinct entries, which decide its sharing mode. Pulled out to be tested on
// its own, because getting it wrong fails only when a validation layer is
// installed, and otherwise builds a silently wrong sharing mode. Not
// installed.

#include <cstdint>

namespace volumetric_kit::core::detail {

// Writes the distinct entries of `families`, in first-seen order, to `out`.
// Returns their count, or `out_capacity + 1` when there are more than `out`
// holds -- an incomplete reduction the caller can only treat as an error.
inline std::uint32_t distinct_queue_families(const std::uint32_t* families,
                                             std::uint32_t count,
                                             std::uint32_t* out,
                                             std::uint32_t out_capacity) {
  std::uint32_t distinct = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    bool seen = false;
    for (std::uint32_t j = 0; j < distinct; ++j) {
      if (out[j] == families[i]) {
        seen = true;
        break;
      }
    }
    if (seen) continue;
    if (distinct == out_capacity) return out_capacity + 1;
    out[distinct++] = families[i];
  }
  return distinct;
}

}  // namespace volumetric_kit::core::detail
