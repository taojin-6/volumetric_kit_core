// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The one translation unit that instantiates the Vulkan Memory Allocator: VMA
// is header-only, and exactly one TU must define VMA_IMPLEMENTATION. No other
// file but allocator.cpp includes <vk_mem_alloc.h>, so VMA never reaches a
// public header or a consumer.
//
// VMA_STATIC_VULKAN_FUNCTIONS resolves entry points against the link-time
// loader the tier already links (vulkan.hpp). Adopting volk for iOS and
// Android would switch this to the dynamic-functions path, here alone.

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

// Through the umbrella, never <vulkan/vulkan.h> directly: VMA needs the
// prototypes in scope before its implementation expands.
#include "volumetric_kit/core/vulkan/vulkan.hpp"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
