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

namespace {

constexpr std::size_t kFeatureCount =
    sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);

const VkBool32* feature_bits(const VkPhysicalDeviceFeatures& features) {
  return reinterpret_cast<const VkBool32*>(&features);
}

}  // namespace

std::string usable_version_text(const PhysicalDeviceInfo& caps) {
  const std::string name = caps.properties().deviceName;
  const std::uint32_t reported = caps.properties().apiVersion;
  if (without_patch(reported) > caps.api_version()) {
    return name + " reports Vulkan " + version_text(reported) +
           ", but the instance negotiated " + version_text(caps.api_version());
  }
  return name + " is a Vulkan " + version_text(caps.api_version()) + " device";
}

bool features_subset(const VkPhysicalDeviceFeatures& wanted,
                     const VkPhysicalDeviceFeatures& have) {
  const VkBool32* want = feature_bits(wanted);
  const VkBool32* got = feature_bits(have);
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    if (want[i] == VK_TRUE && got[i] != VK_TRUE) return false;
  }
  return true;
}

VkPhysicalDeviceFeatures features_union(const VkPhysicalDeviceFeatures& a,
                                        const VkPhysicalDeviceFeatures& b) {
  VkPhysicalDeviceFeatures out{};
  const VkBool32* fa = feature_bits(a);
  const VkBool32* fb = feature_bits(b);
  auto* fo = reinterpret_cast<VkBool32*>(&out);
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    fo[i] = (fa[i] == VK_TRUE || fb[i] == VK_TRUE) ? VK_TRUE : VK_FALSE;
  }
  return out;
}

Status check_physical_support(const PhysicalDeviceInfo& caps,
                              const DeviceRequirements& reqs,
                              const std::vector<std::string>& required) {
  const std::string name = caps.properties().deviceName;
  if (caps.api_version() < reqs.api_version) {
    return Status::unsupported(usable_version_text(caps) + "; Vulkan " +
                               version_text(reqs.api_version) + " is required");
  }
  for (const std::string& extension : required) {
    if (!caps.supports_device_extension(extension.c_str())) {
      return Status::unsupported(std::string(caps.properties().deviceName) +
                                 " does not support the required extension " +
                                 extension);
    }
  }
  if (!features_subset(reqs.features, caps.features())) {
    return Status::unsupported(
        name +
        " does not support a required core feature (DeviceRequirements::"
        "features)");
  }
  // Each is core from a version on, and enabled as core, never through its
  // extension: below that usable version the device may not enable it,
  // whatever it reports.
  struct Flag {
    bool wanted;
    bool supported;
    std::uint32_t core_in;
    const char* feature;
  };
  for (const Flag& flag :
       {Flag{reqs.timeline_semaphore, caps.supports_timeline_semaphore(),
             VK_API_VERSION_1_2, "timelineSemaphore"},
        Flag{reqs.scalar_block_layout, caps.supports_scalar_block_layout(),
             VK_API_VERSION_1_2, "scalarBlockLayout"},
        Flag{reqs.dynamic_rendering, caps.supports_dynamic_rendering(),
             VK_API_VERSION_1_3, "dynamicRendering"}}) {
    if (!flag.wanted) continue;
    if (caps.api_version() < flag.core_in) {
      return Status::unsupported(usable_version_text(caps) + "; " +
                                 flag.feature + " needs Vulkan " +
                                 version_text(flag.core_in));
    }
    if (!flag.supported) {
      return Status::unsupported(name + " does not support " + flag.feature);
    }
  }
  return {};
}

bool can_present(const PhysicalDeviceInfo& caps, std::uint32_t family,
                 VkSurfaceKHR surface) {
  VkBool32 supported = VK_FALSE;
  return vkGetPhysicalDeviceSurfaceSupportKHR(caps.handle(), family, surface,
                                              &supported) == VK_SUCCESS &&
         supported == VK_TRUE;
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

  out.features = detail::features_union(a.features, b.features);
  out.timeline_semaphore = a.timeline_semaphore || b.timeline_semaphore;
  out.scalar_block_layout = a.scalar_block_layout || b.scalar_block_layout;
  out.dynamic_rendering = a.dynamic_rendering || b.dynamic_rendering;
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
  VKC_TRY(detail::check_physical_support(caps, reqs,
                                         detail::required_extensions(reqs)));

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
    // The queue's own family first: one queue for both is the common case,
    // and needs no ownership transfer of the swapchain images.
    if (detail::can_present(caps, *family, surface)) {
      support.present_family = family;
    } else {
      for (std::uint32_t i = 0; i < families.size(); ++i) {
        if (families[i].queueCount > 0 &&
            detail::can_present(caps, i, surface)) {
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
