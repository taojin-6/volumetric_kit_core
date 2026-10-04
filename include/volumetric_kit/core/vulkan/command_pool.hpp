// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_pool.hpp
/// @brief A `VkCommandPool` on one queue family.

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/command_buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Owns a `VkCommandPool` on one queue family and allocates command
///        buffers from it.
///
/// A pool and its buffers must be externally synchronized, so each recording
/// thread takes its own. @ref Device::submit_single_time keeps a pool per
/// submit for the same reason; this is for a caller recording its own
/// buffers, a frame's render pass for one.
///
/// @warning The device must outlive the pool, and the pool every buffer it
///          allocates.
///
/// @code
/// VKC_ASSIGN(CommandPool pool,
///            CommandPool::create(device.handle(), device.queue_family()));
/// VKC_ASSIGN(CommandBuffer cmd, pool.allocate_primary());
/// @endcode
class VKC_VULKAN_API CommandPool {
 public:
  /// @brief Create a pool on @p queue_family.
  /// @param device        The device.
  /// @param queue_family  The family its buffers submit to.
  /// @param flags         Create flags; by default each buffer resets on its
  ///                      own, so it can be re-recorded.
  /// @return The pool; @ref Status::Code::InvalidArgument for a null
  ///         @p device; or a backend @ref Status.
  static Result<CommandPool> create(
      VkDevice device, std::uint32_t queue_family,
      VkCommandPoolCreateFlags flags =
          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

  /// @brief Construct an empty pool; @ref valid is false.
  CommandPool() noexcept = default;
  ~CommandPool() = default;
  CommandPool(CommandPool&& other) noexcept;
  CommandPool& operator=(CommandPool&& other) noexcept;
  CommandPool(const CommandPool&) = delete;
  CommandPool& operator=(const CommandPool&) = delete;

  /// @return The pool (`VK_NULL_HANDLE` when empty).
  VkCommandPool handle() const noexcept { return pool_.get(); }
  /// @return The queue family it was made on.
  std::uint32_t queue_family() const noexcept { return queue_family_; }
  /// @return Whether this owns a pool.
  bool valid() const noexcept { return pool_.valid(); }

  /// @brief Allocate one primary command buffer, which frees itself back here.
  /// @return The buffer; @ref Status::Code::InvalidArgument for an empty pool;
  ///         or a backend @ref Status.
  Result<CommandBuffer> allocate_primary();

 private:
  UniqueHandle<VkCommandPool, vkDestroyCommandPool> pool_;
  std::uint32_t queue_family_ = 0;
};

}  // namespace volumetric_kit::core
