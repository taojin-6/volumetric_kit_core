// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal allocation policy shared by buffers, images and exported memory.
// Budget reads are delayed until an allocation needs new device memory, so
// unused space in existing blocks remains reusable under memory pressure.

#include <cstdint>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

#include <vk_mem_alloc.h>

#include "memory_types.hpp"

namespace volumetric_kit::core::detail {

// `allocate` performs the resource's VMA allocation with these parameters;
// `budgets` returns current heap figures. A resource requiring dedicated
// memory cannot use NEVER_ALLOCATE: VMA rejects that contradictory request.
template <typename Allocate, typename Budgets>
VkResult allocate_with_budget(const VkPhysicalDeviceMemoryProperties& memory,
                              const VkMemoryRequirements& needs,
                              std::uint32_t types, bool mapped,
                              bool dedicated_required, Allocate&& allocate,
                              Budgets&& budgets) {
  VmaAllocationCreateInfo info{};
  info.usage = VMA_MEMORY_USAGE_UNKNOWN;
  info.memoryTypeBits = types;
  info.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  if (mapped) info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;

  if (!dedicated_required) {
    info.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
    const VkResult reused = allocate(info);
    if (reused != VK_ERROR_OUT_OF_DEVICE_MEMORY) return reused;
    info.flags &= ~VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
  }

  // VMA bounds new blocks, but its heuristic dedicated-allocation fallback
  // does not apply WITHIN_BUDGET. Admit that growth explicitly, after reuse
  // has failed. The mask still permits only this resource's placement.
  info.memoryTypeBits =
      types_within_budget(memory, types, needs.size, budgets());
  if (info.memoryTypeBits == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  return allocate(info);
}

}  // namespace volumetric_kit::core::detail
