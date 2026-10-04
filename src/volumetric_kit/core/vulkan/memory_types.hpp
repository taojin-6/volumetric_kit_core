// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: which memory types a placement may use, from a
// device's memory properties alone. Pulled out to be tested against the
// layouts real drivers report -- a discrete GPU's among them, which no CI
// runner has. Not installed.

#include <cstdint>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

// Types no general resource may use: lazily allocated memory is for transient
// attachments, protected memory for protected submissions.
inline constexpr VkMemoryPropertyFlags kSpecialMemory =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT;

// The general types that have all of `required` and none of `excluded`, as a
// memory-type bitmask.
inline std::uint32_t memory_types_with(
    const VkPhysicalDeviceMemoryProperties& props,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded) {
  std::uint32_t mask = 0;
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
    if ((flags & required) == required &&
        (flags & (excluded | kSpecialMemory)) == 0) {
      mask |= 1U << i;
    }
  }
  return mask;
}

// Device-local types the host cannot map: a discrete GPU's VRAM outside its
// BAR window, Apple silicon's GPU-private storage.
inline std::uint32_t device_private_types(
    const VkPhysicalDeviceMemoryProperties& props) {
  return memory_types_with(props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
}

// Every general device-local type, host-visible or not.
inline std::uint32_t device_local_types(
    const VkPhysicalDeviceMemoryProperties& props) {
  return memory_types_with(props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
}

// The types a MemoryUsage::DeviceOnly resource may use: the private ones, or
// -- on a device that has none, where every device-local type is host-visible
// (lavapipe, most integrated and mobile GPUs) -- every device-local type,
// which is the device's one pool. Never a type outside device-local memory,
// so a full VRAM fails rather than spill to host memory or the BAR window.
inline std::uint32_t device_only_types(
    const VkPhysicalDeviceMemoryProperties& props) {
  const std::uint32_t private_types = device_private_types(props);
  return private_types != 0 ? private_types : device_local_types(props);
}

// Whether the GPU and the host share one memory: an integrated or CPU device,
// or one whose every heap is device-local (Apple silicon, lavapipe). A
// discrete GPU has a heap of host memory beside its VRAM. An APU whose driver
// reports a VRAM carve-out beside host memory counts as unified by its type.
inline bool unified_memory(const VkPhysicalDeviceMemoryProperties& props,
                           VkPhysicalDeviceType type) {
  if (type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
      type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
    return true;
  }
  if (props.memoryHeapCount == 0) return false;
  for (std::uint32_t h = 0; h < props.memoryHeapCount; ++h) {
    if ((props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace volumetric_kit::core::detail
