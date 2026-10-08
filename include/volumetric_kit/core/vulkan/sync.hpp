// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file sync.hpp
/// @brief Fences, binary semaphores and timeline semaphores.

#include <atomic>
#include <cstdint>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Device;

namespace detail {
struct TimelineSubmits;  // the submits' record of the values they set
}  // namespace detail

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
/// feature, which @ref DeviceRequirements asks for by default, and which
/// @ref create checks the device enabled.
///
/// @warning The device passed to @ref create must outlive the semaphore.
///
/// @code
/// VKC_ASSIGN(TimelineSemaphore frames, TimelineSemaphore::create(device));
/// VKC_TRY(frames.signal(1));  // the host raises the counter
/// VKC_TRY(frames.wait(1));    // returns once it reaches 1
/// @endcode
class VKC_VULKAN_API TimelineSemaphore {
 public:
  /// @brief Create a timeline semaphore.
  ///
  /// Takes the @ref Device, not its handle, to check
  /// (@ref Device::check_enabled) that it enabled `timelineSemaphore`: a
  /// timeline semaphore on a device that did not is invalid usage the driver
  /// need not report.
  /// @param device         The device.
  /// @param initial_value  The counter's starting value.
  /// @return The semaphore; @ref Status::Code::InvalidArgument for a
  ///         moved-from @p device; @ref Status::Code::Unsupported when it did
  ///         not enable `timelineSemaphore`; or a backend @ref Status.
  static Result<TimelineSemaphore> create(const Device& device,
                                          std::uint64_t initial_value = 0);

  /// @brief Construct an empty semaphore; @ref valid is false.
  TimelineSemaphore() noexcept = default;
  TimelineSemaphore(TimelineSemaphore&& other) noexcept;
  TimelineSemaphore& operator=(TimelineSemaphore&& other) noexcept;
  TimelineSemaphore(const TimelineSemaphore&) = delete;
  TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;
  ~TimelineSemaphore() = default;

  /// @return The semaphore (`VK_NULL_HANDLE` when empty).
  VkSemaphore handle() const noexcept { return handle_.get(); }
  /// @return The `VkDevice` it was made on (`VK_NULL_HANDLE` when empty).
  VkDevice device() const noexcept { return handle_.device(); }
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
  friend struct detail::TimelineSubmits;

  UniqueHandle<VkSemaphore, vkDestroySemaphore> handle_;
  // The highest value a submit that reached a queue sets, as
  // note_timeline_signals records it: a later submit must set a higher one, as
  // a queue's signals run in submission order. Raised through a const
  // semaphore, which a TimelinePoint borrows.
  mutable std::atomic<std::uint64_t> submitted_{0};
};

/// @brief A value of a @ref TimelineSemaphore: one a submission waits for
///        before it starts, or sets once it completes
///        (@ref Device::submit_pending, @ref CommandBatch::submit_async).
///
/// The semaphore is borrowed, and must outlive every submission that names
/// it: one still waiting or running when it is destroyed is undefined.
///
/// @code
/// // The fuse waits for frame n's prep, and says when it is done.
/// VKC_ASSIGN(PendingBatch fused,
///            fuse.submit_async({{&prepared, n}}, {{&fused_timeline, n}}));
/// @endcode
struct TimelinePoint {
  const TimelineSemaphore* semaphore = nullptr;  ///< The timeline.
  std::uint64_t value = 0;                       ///< The value on it.
};

/// @brief Which values a submission may wait for, as
///        @ref check_timeline_points checks them.
enum class TimelineWaits {
  /// Any value, one the host or a later submission sets included, as
  /// @ref Device::submit_pending allows.
  Any,
  /// Only a value already reached, or one a submission that reached a queue
  /// sets (@ref note_timeline_signals): what a submission that a present
  /// waits for needs (`VUID-vkQueuePresentKHR-pWaitSemaphores-03268`).
  Submitted,
};

/// @brief Check the timeline values a submission to @p device waits for and
///        sets, as @ref Device::submit_pending and
///        @ref CommandBatch::submit_async check theirs, for a library that
///        submits to the device's queue itself.
///
/// After such a submission reaches the queue, pass its @p signal to
/// @ref note_timeline_signals, so later checks -- these and the core's own
/// submits' -- know the values it sets. The value checks catch a stale value,
/// not a race, as @ref TimelineSemaphore::signal says.
///
/// @code
/// VKC_TRY(check_timeline_points(device, wait, signal, "Renderer::submit",
///                               TimelineWaits::Submitted));
/// // ... fill VkTimelineSemaphoreSubmitInfo from wait and signal ...
/// if (device.queue_submit(1, &submit, fence) == VK_SUCCESS) {
///   note_timeline_signals(signal);
/// }
/// @endcode
/// @param device  The device the submission goes to.
/// @param wait    Values the submission waits for.
/// @param signal  Values it sets once it completes, one per semaphore.
/// @param call    The caller's name, which a refusal's message begins with.
/// @param waits   Which values @p wait may hold.
/// @return OK; @ref Status::Code::InvalidArgument for a null or empty
///         semaphore, one made on another `VkDevice`, a value to set that
///         would not advance its counter -- one semaphore set twice, a value
///         not above one waited for on the same semaphore, or not above both
///         its counter and every value a recorded submission sets -- or, with
///         @ref TimelineWaits::Submitted, a value to wait for above both its
///         counter and every value a recorded submission sets;
///         @ref Status::Code::Unsupported for a value on a device that did not
///         enable `timelineSemaphore`; or a backend @ref Status from reading
///         a counter. OK for no values at all, on any device.
VKC_VULKAN_API Status check_timeline_points(
    const Device& device, const std::vector<TimelinePoint>& wait,
    const std::vector<TimelinePoint>& signal, const char* call,
    TimelineWaits waits = TimelineWaits::Any);

/// @brief Record that a submission which reached a queue sets each value in
///        @p signal, so @ref check_timeline_points refuses a later one that
///        would not exceed it, and, with @ref TimelineWaits::Submitted,
///        accepts a wait for it.
/// @param signal  The submission's values to set, which
///                @ref check_timeline_points accepted.
VKC_VULKAN_API void note_timeline_signals(
    const std::vector<TimelinePoint>& signal) noexcept;

}  // namespace volumetric_kit::core
