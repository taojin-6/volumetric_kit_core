// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file vulkan.hpp
/// @brief The one header through which first-party code includes Vulkan.
///
/// Every source in the core -- and in recon, gfx and ios once they build on
/// it -- reaches Vulkan through this file, never through `<vulkan/...>`
/// directly; the `vulkan-include-umbrella` pre-commit hook enforces it. The
/// loader choice (the link-time loader today, volk later for iOS and Android)
/// therefore stays a detail of this one line.

#include <vulkan/vulkan.h>  // IWYU pragma: export
