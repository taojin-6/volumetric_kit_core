// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/physical_device_info.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "memory_types.hpp"
#include "support.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

PhysicalDeviceInfo PhysicalDeviceInfo::query(
    VkPhysicalDevice physical, std::uint32_t instance_api_version) {
  PhysicalDeviceInfo info;
  info.physical_ = physical;
  vkGetPhysicalDeviceProperties(physical, &info.properties_);
  // Physical-device functionality of a version needs the device and the
  // instance both to have it.
  info.api_version_ =
      std::min(detail::without_patch(info.properties_.apiVersion),
               detail::without_patch(instance_api_version));
  vkGetPhysicalDeviceMemoryProperties(physical, &info.memory_properties_);
  info.unified_memory_ = detail::unified_memory(info.memory_properties_,
                                                info.properties_.deviceType);

  std::uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
  info.queue_families_.resize(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count,
                                           info.queue_families_.data());
  info.queue_families_.resize(family_count);

  std::uint32_t extension_count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count,
                                           nullptr) == VK_SUCCESS) {
    std::vector<VkExtensionProperties> extensions(extension_count);
    // An enumeration error leaves the list empty, so a missing extension
    // reads as "unsupported" rather than as a crash.
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr,
                                             &extension_count,
                                             extensions.data()) == VK_SUCCESS) {
      extensions.resize(extension_count);
      info.extension_names_.reserve(extensions.size());
      for (const VkExtensionProperties& e : extensions) {
        info.extension_names_.emplace_back(e.extensionName);
      }
    }
  }

  // vkGetPhysicalDeviceFeatures2 is 1.1: below it, the 1.0 query, and the
  // newer features stay unsupported.
  if (info.api_version_ < VK_API_VERSION_1_1) {
    vkGetPhysicalDeviceFeatures(physical, &info.features_);
    return info;
  }
  // The 1.2 and 1.3 feature structs are chained only where the usable
  // version defines them, so the query names no struct the device may not
  // use. The standalone structs, not the version aggregates, because their
  // layout is fixed whatever the header's version.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
  timeline.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  VkPhysicalDeviceScalarBlockLayoutFeatures scalar{};
  scalar.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
  VkPhysicalDeviceDynamicRenderingFeatures dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  void** tail = &features2.pNext;
  if (info.api_version_ >= VK_API_VERSION_1_2) {
    *tail = &timeline;
    timeline.pNext = &scalar;
    tail = &scalar.pNext;
  }
  if (info.api_version_ >= VK_API_VERSION_1_3) {
    *tail = &dynamic;
  }
  vkGetPhysicalDeviceFeatures2(physical, &features2);
  info.features_ = features2.features;
  info.timeline_semaphore_ = timeline.timelineSemaphore == VK_TRUE;
  info.scalar_block_layout_ = scalar.scalarBlockLayout == VK_TRUE;
  info.dynamic_rendering_ = dynamic.dynamicRendering == VK_TRUE;
  return info;
}

bool PhysicalDeviceInfo::supports_device_extension(const char* name) const {
  if (name == nullptr) return false;
  return std::any_of(extension_names_.begin(), extension_names_.end(),
                     [name](const std::string& e) { return e == name; });
}

VkFormatProperties PhysicalDeviceInfo::format_properties(
    VkFormat format) const {
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(physical_, format, &props);
  return props;
}

bool PhysicalDeviceInfo::format_supports(VkFormat format, VkImageTiling tiling,
                                         VkFormatFeatureFlags features) const {
  const VkFormatProperties props = format_properties(format);
  const VkFormatFeatureFlags have = tiling == VK_IMAGE_TILING_OPTIMAL
                                        ? props.optimalTilingFeatures
                                        : props.linearTilingFeatures;
  return (have & features) == features;
}

}  // namespace volumetric_kit::core
