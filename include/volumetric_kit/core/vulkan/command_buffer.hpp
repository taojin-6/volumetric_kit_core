// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_buffer.hpp
/// @brief A primary `VkCommandBuffer`, freed back to its pool.

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Owns a primary `VkCommandBuffer` and frees it back to its pool.
///
/// Made by @ref CommandPool::allocate_primary. Record by passing @ref handle to
/// `vkCmd*` between @ref begin and @ref end, then submit it -- e.g. with
/// @ref Device::submit_and_wait, or as part of a frame's own submit.
///
/// @warning The pool, and the device it belongs to, must outlive the buffer.
///          A pool and its buffers must be externally synchronized: one pool
///          per recording thread.
///
/// @code
/// VKC_ASSIGN(CommandBuffer cmd, pool.allocate_primary());
/// VKC_TRY(cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT));
/// vkCmdFillBuffer(cmd.handle(), buffer, 0, VK_WHOLE_SIZE, 0u);
/// VKC_TRY(cmd.end());
/// VKC_TRY(device.submit_and_wait(cmd.handle()));
/// @endcode
class VKC_VULKAN_API CommandBuffer {
 public:
  /// @brief Construct an empty buffer; @ref valid is false.
  CommandBuffer() noexcept = default;

  /// @brief Adopt @p command_buffer, freed back to @p pool.
  /// @param device          The device @p pool belongs to.
  /// @param pool            The pool it came from.
  /// @param command_buffer  The command buffer.
  CommandBuffer(VkDevice device, VkCommandPool pool,
                VkCommandBuffer command_buffer) noexcept;

  ~CommandBuffer();
  CommandBuffer(CommandBuffer&& other) noexcept;
  CommandBuffer& operator=(CommandBuffer&& other) noexcept;
  CommandBuffer(const CommandBuffer&) = delete;
  CommandBuffer& operator=(const CommandBuffer&) = delete;

  /// @return The command buffer (`VK_NULL_HANDLE` when empty).
  VkCommandBuffer handle() const noexcept { return command_buffer_; }
  /// @return Whether this owns a command buffer.
  bool valid() const noexcept { return command_buffer_ != VK_NULL_HANDLE; }

  /// @brief Begin recording; from a pool made `RESET_COMMAND_BUFFER`, this
  ///        resets the buffer, so it can be re-recorded each frame.
  /// @param flags  `VkCommandBufferUsageFlags`.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty buffer; or a
  ///         backend @ref Status.
  Status begin(VkCommandBufferUsageFlags flags = 0);
  /// @brief Finish recording.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty buffer; or a
  ///         backend @ref Status.
  Status end();

 private:
  void destroy() noexcept;

  VkDevice device_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
};

}  // namespace volumetric_kit::core
