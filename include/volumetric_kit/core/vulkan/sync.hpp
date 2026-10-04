// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file sync.hpp
/// @brief Fences, binary semaphores and timeline semaphores.

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief A `VkFence`: the host waits on it for submitted work to finish.
///
/// @warning The device passed to @ref create must outlive the fence.
///
/// @code
/// VKC_ASSIGN(Fence done, Fence::create(device.handle()));
/// device.queue_submit(1, &submit, done.handle());
/// VKC_TRY(done.wait());
/// @endcode
class VKC_VULKAN_API Fence {
 public:
  /// @brief Create a fence.
  /// @param device    The device.
  /// @param signaled  Whether it starts signaled.
  /// @return The fence; @ref Status::Code::InvalidArgument for a null
  ///         @p device; or a backend @ref Status.
  static Result<Fence> create(VkDevice device, bool signaled = false);

  /// @brief Construct an empty fence; @ref valid is false.
  Fence() noexcept = default;
  Fence(Fence&&) noexcept = default;
  Fence& operator=(Fence&&) noexcept = default;
  Fence(const Fence&) = delete;
  Fence& operator=(const Fence&) = delete;
  ~Fence() = default;

  /// @return The fence (`VK_NULL_HANDLE` when empty).
  VkFence handle() const noexcept { return handle_.get(); }
  /// @return Whether this owns a fence.
  bool valid() const noexcept { return handle_.valid(); }

  /// @brief Wait until the fence is signaled, or the timeout passes.
  /// @param timeout_ns  The longest wait, in nanoseconds.
  /// @return OK once signaled; @ref Status::Code::InvalidArgument for an
  ///         empty fence; a backend @ref Status carrying `VK_TIMEOUT` when
  ///         the timeout passes first, or the failure (device lost).
  Status wait(std::uint64_t timeout_ns = UINT64_MAX) const;
  /// @brief Return the fence to unsignaled.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty fence; or a
  ///         backend @ref Status.
  Status reset();
  /// @return Whether the fence is signaled now; `false` for an empty fence.
  ///         A query error -- device lost included -- also reads as `false`;
  ///         use @ref wait to tell them apart.
  bool is_signaled() const;

 private:
  UniqueHandle<VkFence, vkDestroyFence> handle_;
};

/// @brief A binary `VkSemaphore`: orders one queue submission after another.
///
/// @warning The device passed to @ref create must outlive the semaphore.
///
/// @code
/// VKC_ASSIGN(Semaphore rendered, Semaphore::create(device.handle()));
/// // signalled by one submit, waited on by the next
/// @endcode
class VKC_VULKAN_API Semaphore {
 public:
  /// @brief Create a binary semaphore.
  /// @param device  The device.
  /// @return The semaphore; @ref Status::Code::InvalidArgument for a null
  ///         @p device; or a backend @ref Status.
  static Result<Semaphore> create(VkDevice device);

  /// @brief Construct an empty semaphore; @ref valid is false.
  Semaphore() noexcept = default;
  Semaphore(Semaphore&&) noexcept = default;
  Semaphore& operator=(Semaphore&&) noexcept = default;
  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;
  ~Semaphore() = default;

  /// @return The semaphore (`VK_NULL_HANDLE` when empty).
  VkSemaphore handle() const noexcept { return handle_.get(); }
  /// @return Whether this owns a semaphore.
  bool valid() const noexcept { return handle_.valid(); }

 private:
  UniqueHandle<VkSemaphore, vkDestroySemaphore> handle_;
};

/// @brief A timeline `VkSemaphore`: a 64-bit counter the host and queues both
///        signal and wait on.
///
/// Unlike a binary semaphore, the host reads, signals and waits on the value
/// directly -- the basis for capping frames in flight and for host/GPU
/// hand-offs between libraries. Needs the device's `timelineSemaphore`
/// feature, which @ref DeviceRequirements asks for by default.
///
/// @warning The device passed to @ref create must outlive the semaphore.
///
/// @code
/// VKC_ASSIGN(TimelineSemaphore frames,
///            TimelineSemaphore::create(device.handle()));
/// VKC_TRY(frames.signal(1));  // the host raises the counter
/// VKC_TRY(frames.wait(1));    // returns once it reaches 1
/// @endcode
class VKC_VULKAN_API TimelineSemaphore {
 public:
  /// @brief Create a timeline semaphore.
  /// @param device         The device.
  /// @param initial_value  The counter's starting value.
  /// @return The semaphore; @ref Status::Code::InvalidArgument for a null
  ///         @p device; or a backend @ref Status.
  static Result<TimelineSemaphore> create(VkDevice device,
                                          std::uint64_t initial_value = 0);

  /// @brief Construct an empty semaphore; @ref valid is false.
  TimelineSemaphore() noexcept = default;
  TimelineSemaphore(TimelineSemaphore&&) noexcept = default;
  TimelineSemaphore& operator=(TimelineSemaphore&&) noexcept = default;
  TimelineSemaphore(const TimelineSemaphore&) = delete;
  TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;
  ~TimelineSemaphore() = default;

  /// @return The semaphore (`VK_NULL_HANDLE` when empty).
  VkSemaphore handle() const noexcept { return handle_.get(); }
  /// @return Whether this owns a semaphore.
  bool valid() const noexcept { return handle_.valid(); }

  /// @return The counter's current value; @ref Status::Code::InvalidArgument
  ///         for an empty semaphore; or a backend @ref Status.
  Result<std::uint64_t> value() const;
  /// @brief Raise the counter from the host.
  ///
  /// The check reads the counter, then signals: two steps Vulkan cannot make
  /// one. It catches a caller's stale value, not a race -- another thread's
  /// signal landing between the two, or a value at or past one a queue has
  /// still to signal. Signalling a value that does not advance the counter is
  /// undefined, so when several parties signal one timeline (libraries
  /// handing work over), give each its own values, as a frame index does,
  /// rather than reading the counter and adding one.
  /// @param value  The new value, above the current one and below any value
  ///               a pending queue submission will signal.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty semaphore
  ///         or when @p value does not exceed the value just read; or a
  ///         backend @ref Status.
  Status signal(std::uint64_t value);
  /// @brief Wait until the counter reaches @p value, or the timeout passes.
  /// @param value       The value to wait for.
  /// @param timeout_ns  The longest wait, in nanoseconds.
  /// @return OK once reached; @ref Status::Code::InvalidArgument for an
  ///         empty semaphore; a backend @ref Status carrying `VK_TIMEOUT`
  ///         when the timeout passes first, or the failure.
  Status wait(std::uint64_t value, std::uint64_t timeout_ns = UINT64_MAX) const;

 private:
  UniqueHandle<VkSemaphore, vkDestroySemaphore> handle_;
};

}  // namespace volumetric_kit::core
