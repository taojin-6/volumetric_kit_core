// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// The memory dependencies recorded by CommandBatch. Kept separate from its
// command list so the same recorder can be exercised with a following draw
// in one submission: a host fence wait retires synchronization-validation
// history and can hide an incomplete destination scope.

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

struct BatchScope {
  VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkAccessFlags access = 0;
};

inline BatchScope batch_writes(VkQueueFlags queues) {
  BatchScope scope;
  scope.access = VK_ACCESS_TRANSFER_WRITE_BIT;
  if ((queues & VK_QUEUE_COMPUTE_BIT) != 0) {
    scope.stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    scope.access |= VK_ACCESS_SHADER_WRITE_BIT;
  }
  return scope;
}

inline BatchScope batch_commands(VkQueueFlags queues) {
  BatchScope scope;
  scope.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  if ((queues & VK_QUEUE_COMPUTE_BIT) != 0) {
    scope.stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    scope.access |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_UNIFORM_READ_BIT;
  }
  if ((queues & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) != 0) {
    scope.stages |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    scope.access |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
  }
  return scope;
}

inline void batch_barrier(VkCommandBuffer cmd, const BatchScope& source,
                          const BatchScope& destination) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = source.access;
  barrier.dstAccessMask = destination.access;
  vkCmdPipelineBarrier(cmd, source.stages, destination.stages, 0, 1, &barrier,
                       0, nullptr, 0, nullptr);
}

inline BatchScope batch_consumers(VkQueueFlags queues) {
  BatchScope destination = batch_commands(queues);
  destination.stages |= VK_PIPELINE_STAGE_HOST_BIT;
  destination.access |= VK_ACCESS_HOST_READ_BIT;
  if ((queues & VK_QUEUE_GRAPHICS_BIT) != 0) {
    // A renderer may read an upload through descriptors as well as vertex,
    // index and indirect bindings. ALL_GRAPHICS includes only the enabled
    // graphics stages, so optional shader stages need no feature guesses.
    // The attachment accesses are for images: a dispatch can write a storage
    // image (DescriptorSet::write_storage_image) that the renderer then loads,
    // blends into or overwrites as an attachment.
    destination.stages |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    destination.access |=
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  }
  return destination;
}

inline void batch_final_barrier(VkCommandBuffer cmd, VkQueueFlags queues) {
  batch_barrier(cmd, batch_writes(queues), batch_consumers(queues));
}

}  // namespace volumetric_kit::core::detail
