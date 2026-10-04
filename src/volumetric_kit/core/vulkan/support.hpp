// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: the pieces of the requirements check that
// check_device_support (selection, create) and Device::adopt share. Not
// installed.

#include <cstdint>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

// VK_KHR_portability_subset's name macro lives in vulkan_beta.h, behind
// VK_ENABLE_BETA_EXTENSIONS; the string is stable.
inline constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";

// "1.3" for a packed API version.
std::string version_text(std::uint32_t version);

// reqs.extensions, plus VK_KHR_swapchain when it needs presentation, without
// duplicates and in that order.
std::vector<std::string> required_extensions(const DeviceRequirements& reqs);

// Whether every VK_TRUE bit of `wanted` is VK_TRUE in `have`.
// VkPhysicalDeviceFeatures is a contiguous block of VkBool32, so it compares
// as an array and a new core feature needs no edit here.
bool features_subset(const VkPhysicalDeviceFeatures& wanted,
                     const VkPhysicalDeviceFeatures& have);

// The device-level half of the check: the API version, the required
// extensions, the core features and the feature flags. No queue families.
Status check_physical_support(const PhysicalDeviceInfo& caps,
                              const DeviceRequirements& reqs);

}  // namespace volumetric_kit::core::detail
