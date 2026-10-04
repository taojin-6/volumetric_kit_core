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

// `version` with its patch zeroed, so versions compare as major.minor.
inline std::uint32_t without_patch(std::uint32_t version) {
  return VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(version),
                             VK_API_VERSION_MAJOR(version),
                             VK_API_VERSION_MINOR(version), 0);
}

// reqs.extensions, plus VK_KHR_swapchain when it needs presentation, without
// duplicates and in that order.
std::vector<std::string> required_extensions(const DeviceRequirements& reqs);

// VkPhysicalDeviceFeatures is a contiguous block of VkBool32, so these walk
// it as an array, and a new core feature needs no edit here.

// Whether every VK_TRUE bit of `wanted` is VK_TRUE in `have`.
bool features_subset(const VkPhysicalDeviceFeatures& wanted,
                     const VkPhysicalDeviceFeatures& have);

// Every feature VK_TRUE in `a` or `b`.
VkPhysicalDeviceFeatures features_union(const VkPhysicalDeviceFeatures& a,
                                        const VkPhysicalDeviceFeatures& b);

// The device-level half of the check: the usable API version, the required
// extensions (`required_extensions(reqs)`, built once by the caller, which
// needs them too), the core features and the feature flags. No queue
// families.
Status check_physical_support(const PhysicalDeviceInfo& caps,
                              const DeviceRequirements& reqs,
                              const std::vector<std::string>& required);

}  // namespace volumetric_kit::core::detail
