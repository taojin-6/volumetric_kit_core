// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/command_pool.hpp"

#include <cstdint>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/command_buffer.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<CommandPool> CommandPool::create(VkDevice device,
                                        std::uint32_t queue_family,
                                        VkCommandPoolCreateFlags flags) {
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("CommandPool::create: device is null");
  }
  VkCommandPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  info.flags = flags;
  info.queueFamilyIndex = queue_family;
  VkCommandPool handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateCommandPool(device, &info, nullptr, &handle));
  CommandPool pool;
  pool.pool_ =
      UniqueHandle<VkCommandPool, vkDestroyCommandPool>(device, handle);
  pool.queue_family_ = queue_family;
  return pool;
}

CommandPool::CommandPool(CommandPool&& other) noexcept
    : pool_(std::move(other.pool_)),
      queue_family_(std::exchange(other.queue_family_, 0)) {}

CommandPool& CommandPool::operator=(CommandPool&& other) noexcept {
  if (this != &other) {
    pool_ = std::move(other.pool_);  // frees this pool's handle first
    queue_family_ = std::exchange(other.queue_family_, 0);
  }
  return *this;
}

Result<CommandBuffer> CommandPool::allocate_primary() {
  if (!valid()) {
    return Status::invalid_argument(
        "CommandPool::allocate_primary on an empty pool");
  }
  VkCommandBufferAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  info.commandPool = pool_.get();
  info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  info.commandBufferCount = 1;
  VkCommandBuffer handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkAllocateCommandBuffers(pool_.device(), &info, &handle));
  return CommandBuffer(pool_.device(), pool_.get(), handle);
}

}  // namespace volumetric_kit::core
