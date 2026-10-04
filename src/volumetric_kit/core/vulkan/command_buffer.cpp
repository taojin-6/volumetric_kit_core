// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/command_buffer.hpp"

#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

CommandBuffer::CommandBuffer(VkDevice device, VkCommandPool pool,
                             VkCommandBuffer command_buffer) noexcept
    : device_(device), pool_(pool), command_buffer_(command_buffer) {}

CommandBuffer::~CommandBuffer() { destroy(); }

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      command_buffer_(std::exchange(other.command_buffer_, VK_NULL_HANDLE)) {}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept {
  if (this != &other) {
    destroy();
    device_ = std::exchange(other.device_, VK_NULL_HANDLE);
    pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
    command_buffer_ = std::exchange(other.command_buffer_, VK_NULL_HANDLE);
  }
  return *this;
}

void CommandBuffer::destroy() noexcept {
  if (command_buffer_ != VK_NULL_HANDLE) {
    vkFreeCommandBuffers(device_, pool_, 1, &command_buffer_);
  }
  device_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  command_buffer_ = VK_NULL_HANDLE;
}

Status CommandBuffer::begin(VkCommandBufferUsageFlags flags) {
  if (command_buffer_ == VK_NULL_HANDLE) {
    return Status::invalid_argument("CommandBuffer::begin on an empty buffer");
  }
  VkCommandBufferBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  info.flags = flags;
  VKC_VK_TRY(vkBeginCommandBuffer(command_buffer_, &info));
  return {};
}

Status CommandBuffer::end() {
  if (command_buffer_ == VK_NULL_HANDLE) {
    return Status::invalid_argument("CommandBuffer::end on an empty buffer");
  }
  VKC_VK_TRY(vkEndCommandBuffer(command_buffer_));
  return {};
}

}  // namespace volumetric_kit::core
