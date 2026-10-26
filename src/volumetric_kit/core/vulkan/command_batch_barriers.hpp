// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// The memory dependencies recorded by CommandBatch. Kept separate from its
// command list so the same recorder can be exercised with a draw before or
// after it in one submission: a host fence wait retires
// synchronization-validation history and can hide an incomplete scope.

#include <cstdint>

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

// What a release waits for: every use of the resource on this queue before
// it -- the batch's commands, and any work since the batch that acquired
// it, draws included, which the final barrier lets follow that batch -- and
// the writes among them, which it makes available. Not the host's: the
// submit orders those.
inline BatchScope batch_users(VkQueueFlags queues) {
  BatchScope source = batch_consumers(queues);
  source.stages &= ~VkPipelineStageFlags{VK_PIPELINE_STAGE_HOST_BIT};
  source.access &= VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  return source;
}

// One half of a queue-family ownership transfer: of a whole buffer, or of
// every mip and layer of an image, whose contents move between two layouts.
struct BatchTransfer {
  VkBuffer buffer = VK_NULL_HANDLE;  // or
  VkImage image = VK_NULL_HANDLE;
  VkImageAspectFlags aspect = 0;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  std::uint32_t from = VK_QUEUE_FAMILY_IGNORED;
  std::uint32_t to = VK_QUEUE_FAMILY_IGNORED;
};

inline void batch_transfer_barrier(VkCommandBuffer cmd,
                                   const BatchTransfer& transfer,
                                   VkPipelineStageFlags source_stages,
                                   VkAccessFlags source_access,
                                   VkPipelineStageFlags destination_stages,
                                   VkAccessFlags destination_access) {
  if (transfer.image != VK_NULL_HANDLE) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = source_access;
    b.dstAccessMask = destination_access;
    b.oldLayout = transfer.old_layout;
    b.newLayout = transfer.new_layout;
    b.srcQueueFamilyIndex = transfer.from;
    b.dstQueueFamilyIndex = transfer.to;
    b.image = transfer.image;
    b.subresourceRange = {transfer.aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                          VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cmd, source_stages, destination_stages, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    return;
  }
  VkBufferMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  b.srcAccessMask = source_access;
  b.dstAccessMask = destination_access;
  b.srcQueueFamilyIndex = transfer.from;
  b.dstQueueFamilyIndex = transfer.to;
  b.buffer = transfer.buffer;
  b.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, source_stages, destination_stages, 0, 0, nullptr, 1,
                       &b, 0, nullptr);
}

// The acquiring half. The releasing half made the writes available, so this
// makes them, and an image's new layout, visible to every command after it,
// and waits on nothing before it.
inline void batch_acquire(VkCommandBuffer cmd, const BatchScope& commands,
                          const BatchTransfer& transfer) {
  batch_transfer_barrier(cmd, transfer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         commands.stages, commands.access);
}

// The releasing half, recorded after every command. It waits for every use
// before it (batch_users), then moves an image's contents to their new
// layout -- a write those uses' reads must precede -- and makes their writes
// available to the family the resource goes to, whose acquire makes them
// visible.
inline void batch_release(VkCommandBuffer cmd, VkQueueFlags queues,
                          const BatchTransfer& transfer) {
  const BatchScope users = batch_users(queues);
  batch_transfer_barrier(cmd, transfer, users.stages, users.access,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
}

}  // namespace volumetric_kit::core::detail
