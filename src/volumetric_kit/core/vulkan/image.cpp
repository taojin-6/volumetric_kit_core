// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/image.hpp"

#include <functional>
#include <utility>

namespace volumetric_kit::core {

Image::Image(const ImageInfo& info, std::function<void()> deleter) noexcept
    : info_(info), deleter_(std::move(deleter)) {
  if (info_.image == VK_NULL_HANDLE) info_.memory.reset();
}

Image::~Image() { destroy(); }

Image::Image(Image&& other) noexcept
    : info_(std::exchange(other.info_, ImageInfo{})),
      deleter_(std::exchange(other.deleter_, nullptr)) {}

Image& Image::operator=(Image&& other) noexcept {
  if (this != &other) {
    destroy();
    info_ = std::exchange(other.info_, ImageInfo{});
    deleter_ = std::exchange(other.deleter_, nullptr);
  }
  return *this;
}

// A deleter frees handles and does not throw; one that did would terminate
// here, as it should from a destructor.
// NOLINTNEXTLINE(bugprone-exception-escape)
void Image::destroy() noexcept {
  if (deleter_) deleter_();
  info_ = ImageInfo{};
  deleter_ = nullptr;
}

}  // namespace volumetric_kit::core
