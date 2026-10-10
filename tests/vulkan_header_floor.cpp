// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Compiled, never linked or run, by the vkc_vulkan_header_floor_* tests
// (tests/CMakeLists.txt): the system's Vulkan headers, with VK_HEADER_VERSION
// replaced by VKC_TEST_HEADER_VERSION, so vulkan.hpp's floor check judges
// that version. Below the floor it must refuse; at the floor it must accept.

#include <vulkan/vulkan.h>

#undef VK_HEADER_VERSION
#define VK_HEADER_VERSION VKC_TEST_HEADER_VERSION

#include "volumetric_kit/core/vulkan/vulkan.hpp"
