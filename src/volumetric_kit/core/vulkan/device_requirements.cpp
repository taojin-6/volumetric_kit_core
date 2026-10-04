// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/device_requirements.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "support.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"

namespace volumetric_kit::core {
namespace detail {

std::string version_text(std::uint32_t version) {
  return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
         std::to_string(VK_API_VERSION_MINOR(version));
}

std::vector<std::string> required_extensions(const DeviceRequirements& reqs) {
  std::vector<std::string> out;
  auto add = [&out](const std::string& name) {
    if (std::find(out.begin(), out.end(), name) == out.end()) {
      out.push_back(name);
    }
  };
  for (const std::string& name : reqs.extensions) add(name);
  if (reqs.needs_present) add(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  return out;
}

bool features_subset(const VkPhysicalDeviceFeatures& wanted,
                     const VkPhysicalDeviceFeatures& have) {
  constexpr std::size_t kCount =
      sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
  const auto* want = reinterpret_cast<const VkBool32*>(&wanted);
  const auto* got = reinterpret_cast<const VkBool32*>(&have);
  for (std::size_t i = 0; i < kCount; ++i) {
    if (want[i] == VK_TRUE && got[i] != VK_TRUE) return false;
  }
  return true;
}

Status check_physical_support(const PhysicalDeviceInfo& caps,
                              const DeviceRequirements& reqs) {
  if (caps.api_version() < reqs.api_version) {
    return Status::unsupported(
        std::string(caps.properties().deviceName) + " is a Vulkan " +
        version_text(caps.api_version()) + " device; Vulkan " +
        version_text(reqs.api_version) + " is required");
  }
  for (const std::string& name : required_extensions(reqs)) {
    if (!caps.supports_device_extension(name.c_str())) {
      return Status::unsupported(std::string(caps.properties().deviceName) +
                                 " does not support the required extension " +
                                 name);
    }
  }
  if (!features_subset(reqs.features, caps.features())) {
    return Status::unsupported(
        std::string(caps.properties().deviceName) +
        " does not support a required core feature (DeviceRequirements::"
        "features)");
  }
  if (reqs.timeline_semaphore && !caps.supports_timeline_semaphore()) {
    return Status::unsupported(std::string(caps.properties().deviceName) +
                               " does not support timelineSemaphore");
  }
  if (reqs.scalar_block_layout && !caps.supports_scalar_block_layout()) {
    return Status::unsupported(std::string(caps.properties().deviceName) +
                               " does not support scalarBlockLayout");
  }
  if (reqs.dynamic_rendering && !caps.supports_dynamic_rendering()) {
    return Status::unsupported(std::string(caps.properties().deviceName) +
                               " does not support dynamicRendering");
  }
  return {};
}

}  // namespace detail

Result<DeviceRequirements> merge(const DeviceRequirements& a,
                                 const DeviceRequirements& b) {
  if (a.feature_chain != nullptr && b.feature_chain != nullptr &&
      a.feature_chain != b.feature_chain) {
    return Status::invalid_argument(
        "merge: both requirements carry a feature_chain; link the two chains "
        "into one and set it on one of them");
  }
  DeviceRequirements out;
  out.api_version = std::max(a.api_version, b.api_version);
  out.queue_flags = a.queue_flags | b.queue_flags;
  out.needs_present = a.needs_present || b.needs_present;

  auto add_unique = [](std::vector<std::string>& list,
                       const std::string& name) {
    if (std::find(list.begin(), list.end(), name) == list.end()) {
      list.push_back(name);
    }
  };
  for (const std::string& name : a.extensions) add_unique(out.extensions, name);
  for (const std::string& name : b.extensions) add_unique(out.extensions, name);
  // An extension one side requires is required, whatever the other says.
  for (const auto* side : {&a, &b}) {
    for (const std::string& name : side->optional_extensions) {
      if (std::find(out.extensions.begin(), out.extensions.end(), name) ==
          out.extensions.end()) {
        add_unique(out.optional_extensions, name);
      }
    }
  }

  constexpr std::size_t kCount =
      sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
  const auto* fa = reinterpret_cast<const VkBool32*>(&a.features);
  const auto* fb = reinterpret_cast<const VkBool32*>(&b.features);
  auto* fo = reinterpret_cast<VkBool32*>(&out.features);
  for (std::size_t i = 0; i < kCount; ++i) {
    fo[i] = (fa[i] == VK_TRUE || fb[i] == VK_TRUE) ? VK_TRUE : VK_FALSE;
  }

  out.timeline_semaphore = a.timeline_semaphore || b.timeline_semaphore;
  out.scalar_block_layout = a.scalar_block_layout || b.scalar_block_layout;
  out.dynamic_rendering = a.dynamic_rendering || b.dynamic_rendering;
  out.debug_utils = a.debug_utils || b.debug_utils;
  out.feature_chain =
      a.feature_chain != nullptr ? a.feature_chain : b.feature_chain;
  return out;
}

Result<DeviceSupport> check_device_support(const PhysicalDeviceInfo& caps,
                                           const DeviceRequirements& reqs,
                                           VkSurfaceKHR surface) {
  if (caps.handle() == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "check_device_support: no physical device captured");
  }
  if (reqs.needs_present && surface == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "check_device_support: needs_present requires a surface");
  }
  VKC_TRY(detail::check_physical_support(caps, reqs));

  const std::vector<VkQueueFamilyProperties>& families = caps.queue_families();
  std::optional<std::uint32_t> family;
  for (std::uint32_t i = 0; i < families.size(); ++i) {
    if (families[i].queueCount > 0 &&
        (families[i].queueFlags & reqs.queue_flags) == reqs.queue_flags) {
      family = i;
      break;
    }
  }
  if (!family) {
    return Status::unsupported(
        std::string(caps.properties().deviceName) +
        " has no queue family with every required capability "
        "(DeviceRequirements::queue_flags)");
  }

  DeviceSupport support;
  support.queue_family = *family;
  if (reqs.needs_present) {
    // A failed query reads as "cannot present" rather than trusting an
    // unwritten result.
    auto can_present = [&](std::uint32_t index) {
      VkBool32 supported = VK_FALSE;
      return vkGetPhysicalDeviceSurfaceSupportKHR(caps.handle(), index, surface,
                                                  &supported) == VK_SUCCESS &&
             supported == VK_TRUE;
    };
    // The queue's own family first: one queue for both is the common case,
    // and needs no ownership transfer of the swapchain images.
    if (can_present(*family)) {
      support.present_family = family;
    } else {
      for (std::uint32_t i = 0; i < families.size(); ++i) {
        if (families[i].queueCount > 0 && can_present(i)) {
          support.present_family = i;
          break;
        }
      }
    }
    if (!support.present_family) {
      return Status::unsupported(std::string(caps.properties().deviceName) +
                                 " has no queue family that can present to "
                                 "the surface");
    }
  }
  return support;
}

}  // namespace volumetric_kit::core
