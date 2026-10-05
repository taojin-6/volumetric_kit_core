// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: the pieces of the requirements check that
// check_device_support (selection, create), Device::adopt and
// Device::check_enabled share, and the device selection and creation
// Device::create and SharedDevice share, so the two ways of making a device
// cannot drift. Not installed.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
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

// VkPhysicalDeviceFeatures is a contiguous block of VkBool32, frozen at
// Vulkan 1.0, so these walk it as an array.

// The name of the first feature VK_TRUE in `wanted` and not in `have`, as
// VkPhysicalDeviceFeatures spells it; null when `have` has them all.
const char* first_missing_feature(const VkPhysicalDeviceFeatures& wanted,
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

// The extensions a device made for `reqs` enables: the required ones, then
// the portability subset (which the spec requires wherever it is exposed),
// VK_EXT_memory_budget, and `reqs.optional_extensions`, each where `caps`
// offers it.
std::vector<std::string> enabled_extensions(const PhysicalDeviceInfo& caps,
                                            const DeviceRequirements& reqs);

// Whether queue family `family` of `caps` can present to `surface`. A failed
// query reads as "cannot present" rather than trusting an unwritten result.
bool can_present(const PhysicalDeviceInfo& caps, std::uint32_t family,
                 VkSurfaceKHR surface);

// A queue family, and how many of its queues a device is created with.
struct QueueRequest {
  std::uint32_t family = 0;
  std::uint32_t count = 1;
};

// A device create_device made, and what it enabled of the features
// DeviceRequirements names: the requirements' own, and any of the three
// flags the caller's feature chain set itself.
struct CreatedDevice {
  VkDevice device = VK_NULL_HANDLE;
  EnabledFeatures enabled;
};

// The one vkCreateDevice path Device::create and SharedDevice share: on
// `caps`, for `reqs` -- its core features, timeline semaphores, scalar block
// layout and dynamic rendering, then its feature chain, whose structs may be
// written (DeviceRequirements::feature_chain) -- enabling `extensions`
// (enabled_extensions(caps, reqs)) and the queues `queues` asks for: one
// create info per distinct family, with the most queues any request asks of
// it.
Result<CreatedDevice> create_device(const PhysicalDeviceInfo& caps,
                                    const DeviceRequirements& reqs,
                                    const std::vector<std::string>& extensions,
                                    const std::vector<QueueRequest>& queues);

// Instance::select_physical_device, with `accept` asked of each device that
// meets `reqs` too: one it refuses is passed over, and its reason joins the
// others when no device qualifies. Null accepts every device.
Result<PhysicalDeviceInfo> select_physical_device(
    const Instance& instance, const DeviceRequirements& reqs,
    VkSurfaceKHR surface,
    const std::function<Status(const PhysicalDeviceInfo&)>& accept);

}  // namespace volumetric_kit::core::detail
