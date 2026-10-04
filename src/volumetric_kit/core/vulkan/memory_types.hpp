// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: which memory types a placement may use, from a
// device's memory properties alone. Pulled out to be tested against the
// layouts real drivers report -- a discrete GPU's among them, which no CI
// runner has. Not installed.

#include <cstdint>
#include <optional>

#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

// Types no general resource may use: lazily allocated memory is for transient
// attachments, protected memory for protected submissions, and
// device-coherent and device-uncached memory (VK_AMD_device_coherent_memory)
// for markers read while the device runs -- a feature this tier never
// enables, and memory VMA leaves out of every allocation unless its allocator
// opts in.
inline constexpr VkMemoryPropertyFlags kSpecialMemory =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT |
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;

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
// BAR window, or GPU-private storage on unified memory.
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

// Whether the GPU and the host share one memory: every heap is device-local,
// so every type is too -- the spec sets DEVICE_LOCAL on a type exactly when
// its heap has it. A discrete GPU has a heap of host memory beside its VRAM,
// and so does an APU whose driver reports a VRAM carve-out beside host memory:
// there the carve-out is the device-local memory, filled and refused as VRAM
// is.
inline bool unified_memory(const VkPhysicalDeviceMemoryProperties& props) {
  if (props.memoryHeapCount == 0) return false;
  for (std::uint32_t h = 0; h < props.memoryHeapCount; ++h) {
    if ((props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
      return false;
    }
  }
  return true;
}

// The types a MemoryUsage::DeviceOnly resource may use: the private ones, or
// every device-local type on a device with no private types. Never a type
// outside device-local memory, and never a mapped type when a private
// placement exists on a discrete device.
inline std::uint32_t device_only_types(
    const VkPhysicalDeviceMemoryProperties& props) {
  const std::uint32_t private_types = device_private_types(props);
  return private_types != 0 ? private_types : device_local_types(props);
}

// The types a MemoryUsage::DeviceMapped resource may use: device-local and
// mapped coherently -- a discrete GPU's BAR window (all of VRAM under
// Resizable BAR), the one pool of unified memory. 0 when the device has none;
// never a type outside device-local memory.
inline std::uint32_t device_mapped_types(
    const VkPhysicalDeviceMemoryProperties& props) {
  return memory_types_with(props,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           0);
}

// The types a MemoryUsage::Staging buffer may use: host memory the device
// does not hold -- a discrete GPU's system RAM -- so staging never takes VRAM
// or the BAR window; on a device with none (unified memory), every coherent
// host-visible type.
inline std::uint32_t staging_types(
    const VkPhysicalDeviceMemoryProperties& props) {
  constexpr VkMemoryPropertyFlags kMapped =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const std::uint32_t host =
      memory_types_with(props, kMapped, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  return host != 0 ? host : memory_types_with(props, kMapped, 0);
}

// The types one resource may take for `memory`, given the types its memory
// requirements allow (`allowed`): the placement's mask cut to the resource,
// so a type the resource cannot use never decides anything. 0 when none
// suits, which the allocator refuses rather than place the resource
// elsewhere.
//
// - DeviceOnly: the device-only types the resource allows. Where it allows
//   none -- a linear image limited to host-visible types, for example --
//   unified memory takes any device-local type, as all of them are the one
//   pool; a discrete GPU takes none, as the rest of its device-local memory is
//   the BAR window.
// - DeviceMapped and Staging: narrowed by `access`. SequentialWrite prefers
//   write-combined (uncached) types and takes cached ones where there are
//   none; Random prefers cached types, and for DeviceMapped requires them, so
//   uncached mapped device memory is refused rather than read across an
//   interconnect uncached.
inline std::uint32_t placement_types(
    const VkPhysicalDeviceMemoryProperties& props, MemoryUsage memory,
    HostAccess access, std::uint32_t allowed) {
  if (memory == MemoryUsage::DeviceOnly) {
    const std::uint32_t types = device_only_types(props) & allowed;
    if (types != 0 || !unified_memory(props)) return types;
    return device_local_types(props) & allowed;
  }
  const std::uint32_t types = allowed & (memory == MemoryUsage::DeviceMapped
                                             ? device_mapped_types(props)
                                             : staging_types(props));
  const std::uint32_t cached =
      types & memory_types_with(props, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
  if (access == HostAccess::SequentialWrite) {
    return (types & ~cached) != 0 ? (types & ~cached) : types;
  }
  if (memory == MemoryUsage::DeviceMapped) return cached;
  return cached != 0 ? cached : types;
}

// find_memory_type's search: the first type `type_bits` allows with every
// flag of `required` and none of `excluded`. Device-local memory asked for
// without HOST_VISIBLE is cut as DeviceOnly is, so it never takes a discrete
// GPU's BAR window. Empty when no type suits.
inline std::optional<std::uint32_t> first_memory_type(
    const VkPhysicalDeviceMemoryProperties& props, std::uint32_t type_bits,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded) {
  std::uint32_t allowed =
      memory_types_with(props, required, excluded) & type_bits;
  if ((required & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
      (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
    // The host access is ignored for device-only memory.
    allowed &= placement_types(props, MemoryUsage::DeviceOnly,
                               HostAccess::SequentialWrite, type_bits);
  }
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((allowed & (1U << i)) != 0) return i;
  }
  return std::nullopt;
}

// The types of `types` whose heap has room for `bytes` more within its
// budget, by an Allocator's `heaps` figures. Used only to admit new memory:
// existing blocks already count against the heap's usage.
inline std::uint32_t types_within_budget(
    const VkPhysicalDeviceMemoryProperties& props, std::uint32_t types,
    VkDeviceSize bytes, const MemoryStats& heaps) {
  std::uint32_t fit = 0;
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const std::uint32_t heap = props.memoryTypes[i].heapIndex;
    if ((types & (1U << i)) == 0 || heap >= heaps.heap_count) continue;
    const HeapStats& figures = heaps.heaps[heap];
    if (bytes <= figures.budget_bytes &&
        figures.usage_bytes <= figures.budget_bytes - bytes) {
      fit |= 1U << i;
    }
  }
  return fit;
}

}  // namespace volumetric_kit::core::detail
