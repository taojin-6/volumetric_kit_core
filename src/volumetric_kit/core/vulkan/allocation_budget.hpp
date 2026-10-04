// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal allocation policy shared by buffers, images and exported memory:
// new device memory only where a heap's budget has room, and at the budget
// only the unused space of blocks the allocator already holds.

#include <cstdint>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

#include <vk_mem_alloc.h>

#include "memory_types.hpp"

namespace volumetric_kit::core::detail {

// An Allocator's figures from VMA's: each heap's usage and budget -- the
// driver's estimate for the process where VK_EXT_memory_budget is enabled --
// and this allocator's own blocks and allocations in it.
inline MemoryStats memory_stats_of(const VmaBudget* budgets,
                                   std::uint32_t heap_count) {
  MemoryStats stats;
  stats.heap_count = heap_count;
  for (std::uint32_t heap = 0; heap < heap_count; ++heap) {
    stats.heaps[heap].usage_bytes = budgets[heap].usage;
    stats.heaps[heap].budget_bytes = budgets[heap].budget;
    stats.heaps[heap].reserved_bytes = budgets[heap].statistics.blockBytes;
    stats.heaps[heap].allocation_bytes =
        budgets[heap].statistics.allocationBytes;
  }
  return stats;
}

// How an allocation ended: VMA's result, and whether a refusal was the
// budget's -- no candidate heap had room for new memory, and no block had
// room for the resource -- rather than VMA's or the driver's.
struct BudgetedAllocation {
  VkResult result = VK_SUCCESS;
  bool over_budget = false;
};

// Allocates a resource of `needs` in `types`, the mask placement_types cut
// for it. `allocate` makes the resource's VMA call with the parameters it is
// given; `budgets` returns the allocator's current figures.
//
// Where a candidate heap has room for the resource, VMA allocates as it would
// without a budget: from an existing block, else a new block of the first
// candidate type, or memory of the resource's own where the driver prefers
// that or the resource is large. WITHIN_BUDGET bounds VMA's new blocks but not
// that dedicated path, so the mask is first cut to the heaps with room.
//
// At the budget no new memory is made, but a block's unused space already
// counts against its heap, so a resource that fits in one still takes it, in
// any candidate type. Memory a resource requires to itself is never reused.
template <typename Allocate, typename Budgets>
BudgetedAllocation allocate_with_budget(
    const VkPhysicalDeviceMemoryProperties& memory,
    const VkMemoryRequirements& needs, std::uint32_t types, bool mapped,
    bool dedicated_required, Allocate&& allocate, Budgets&& budgets) {
  VmaAllocationCreateInfo info{};
  info.usage = VMA_MEMORY_USAGE_UNKNOWN;
  info.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  if (mapped) info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;

  const std::uint32_t admitted =
      types_within_budget(memory, types, needs.size, budgets());
  if (admitted != 0) {
    info.memoryTypeBits = admitted;
    const VkResult made = allocate(info);
    if (made != VK_ERROR_OUT_OF_DEVICE_MEMORY) return {made, false};
  }
  if (dedicated_required) {
    return {VK_ERROR_OUT_OF_DEVICE_MEMORY, admitted == 0};
  }
  info.memoryTypeBits = types;
  info.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
  const VkResult reused = allocate(info);
  return {reused, reused == VK_ERROR_OUT_OF_DEVICE_MEMORY && admitted == 0};
}

}  // namespace volumetric_kit::core::detail
