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
    : deleter_(std::move(deleter)) {
  state_.buffer = handle;
  state_.size = size;
  state_.usage = usage;
  state_.sharing = sharing;
  state_.mapped = mapped;
  if (handle != VK_NULL_HANDLE) state_.memory = memory;
}

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer&& other) noexcept
    : state_(std::exchange(other.state_, State{})),
      deleter_(std::exchange(other.deleter_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    destroy();
    state_ = std::exchange(other.state_, State{});
    deleter_ = std::exchange(other.deleter_, nullptr);
  }
  return *this;
}

// A deleter frees handles and does not throw; one that did would terminate
// here, as it should from a destructor.
// NOLINTNEXTLINE(bugprone-exception-escape)
void Buffer::destroy() noexcept {
  if (deleter_) deleter_();
  state_ = State{};
  deleter_ = nullptr;
}

}  // namespace volumetric_kit::core
