// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The consumer's installed library: a function returning the core's Result, so
// the core is part of its public interface, as in every sibling.

#include "volumetric_kit/core/base/result.hpp"

namespace vkc = volumetric_kit::core;

vkc::Result<int> parse_positive(int x) {
  if (x <= 0) return vkc::Status::invalid_argument("not positive");
  return x;
}
