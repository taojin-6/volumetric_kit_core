// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/buffer.hpp"

#include <functional>
#include <optional>
#include <utility>

namespace volumetric_kit::core {

Buffer::Buffer(VkBuffer handle, VkDeviceSize size, VkBufferUsageFlags usage,
               VkSharingMode sharing, void* mapped,
               std::function<void()> deleter,
               std::optional<MemoryInfo> memory) noexcept
    : buffer_(handle),
      size_(size),
      usage_(usage),
      sharing_(sharing),
      mapped_(mapped),
      memory_(handle == VK_NULL_HANDLE ? std::nullopt : memory),
      deleter_(std::move(deleter)) {}

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer&& other) noexcept
    : buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)),
      usage_(std::exchange(other.usage_, 0)),
      sharing_(std::exchange(other.sharing_, VK_SHARING_MODE_EXCLUSIVE)),
      mapped_(std::exchange(other.mapped_, nullptr)),
      memory_(std::exchange(other.memory_, std::nullopt)),
      deleter_(std::exchange(other.deleter_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    destroy();
    buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
    size_ = std::exchange(other.size_, 0);
    usage_ = std::exchange(other.usage_, 0);
    sharing_ = std::exchange(other.sharing_, VK_SHARING_MODE_EXCLUSIVE);
    mapped_ = std::exchange(other.mapped_, nullptr);
    memory_ = std::exchange(other.memory_, std::nullopt);
    deleter_ = std::exchange(other.deleter_, nullptr);
  }
  return *this;
}

// A deleter frees handles and does not throw; one that did would terminate
// here, as it should from a destructor.
// NOLINTNEXTLINE(bugprone-exception-escape)
void Buffer::destroy() noexcept {
  if (deleter_) deleter_();
  buffer_ = VK_NULL_HANDLE;
  size_ = 0;
  usage_ = 0;
  sharing_ = VK_SHARING_MODE_EXCLUSIVE;
  mapped_ = nullptr;
  memory_.reset();
  deleter_ = nullptr;
}

}  // namespace volumetric_kit::core
