// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file unique_handle.hpp
/// @brief Sole owner of a device-scoped Vulkan handle, freeing it via a
///        `vkDestroy*` entry point exactly once.

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Owns a Vulkan @p HandleT created against a `VkDevice` and frees it
///        via @p Destroy exactly once.
///
/// Collapses the device-plus-handle move/reset/destroy bookkeeping that every
/// device-owned wrapper (fences, semaphores, shader modules, descriptor
/// layouts, pipelines) would otherwise re-derive -- where a forgotten reset
/// double-frees or leaks -- into one owner. The deleter is a compile-time
/// `vkDestroy*` pointer, so the owner adds no storage beyond the device and
/// the handle. recon and gfx carried identical copies of it.
///
/// @warning The `VkDevice` the handle was created on must outlive this owner:
///          the destructor calls @p Destroy on the stored device.
///
/// @tparam HandleT  The Vulkan handle type (e.g. `VkShaderModule`).
/// @tparam Destroy  The `vkDestroy*` entry point that frees a @p HandleT.
///
/// @code
/// UniqueHandle<VkFence, vkDestroyFence> fence(device, raw_fence);  // adopts
/// VkFence h = fence.get();
/// @endcode
template <class HandleT, void(VKAPI_PTR* Destroy)(VkDevice, HandleT,
                                                  const VkAllocationCallbacks*)>
class UniqueHandle {
 public:
  /// @brief Construct an empty owner (owns nothing; `valid()` is false).
  UniqueHandle() noexcept = default;

  /// @brief Adopt @p handle, owned against @p device and freed by @p Destroy.
  /// @param device  The device @p handle was created on.
  /// @param handle  The handle to take ownership of.
  UniqueHandle(VkDevice device, HandleT handle) noexcept
      : device_(device), handle_(handle) {}

  ~UniqueHandle() { destroy(); }

  UniqueHandle(UniqueHandle&& other) noexcept
      : device_(other.device_), handle_(other.handle_) {
    other.device_ = VK_NULL_HANDLE;
    other.handle_ = VK_NULL_HANDLE;
  }

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      destroy();
      device_ = other.device_;
      handle_ = other.handle_;
      other.device_ = VK_NULL_HANDLE;
      other.handle_ = VK_NULL_HANDLE;
    }
    return *this;
  }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  /// @return The owned handle (`VK_NULL_HANDLE` when empty).
  HandleT get() const noexcept { return handle_; }

  /// @return The device the handle was created against.
  VkDevice device() const noexcept { return device_; }

  /// @return `true` if this owns a handle.
  bool valid() const noexcept { return handle_ != VK_NULL_HANDLE; }

 private:
  void destroy() noexcept {
    if (handle_ != VK_NULL_HANDLE) {
      Destroy(device_, handle_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    handle_ = VK_NULL_HANDLE;
  }

  VkDevice device_ = VK_NULL_HANDLE;
  HandleT handle_ = VK_NULL_HANDLE;
};

}  // namespace volumetric_kit::core
