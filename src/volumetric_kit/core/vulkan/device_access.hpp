// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: what the tier reaches inside a Device -- its
// counters and its calibrated clock -- and the blocking waits, which report
// the time they block to the scopes open on their thread (note_blocked) and
// to those counters. Not installed.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

// DeviceCounters, as atomics a Device and its timeline semaphores share.
struct DeviceCounterState {
  std::atomic<std::uint64_t> submits{0};
  std::atomic<std::uint64_t> host_waits{0};
  std::atomic<std::uint64_t> blocked_ns{0};
  std::atomic<std::uint64_t> staging_buffers{0};
  std::atomic<std::uint64_t> dispatches{0};
  std::atomic<std::uint64_t> barriers{0};
};

// Preallocated by a resource whose failed work may have no fence known to
// Device. Linking it here never allocates; Device retires the list only once
// its assigned queue is idle (including for an adopted device).
struct RetainedResource {
  std::shared_ptr<void> resource;
  std::unique_ptr<RetainedResource> next;
};

struct DeviceRetainedResources {
  std::mutex mutex;
  std::unique_ptr<RetainedResource> head;
};

// A device's calibrated-timestamps entry point and time domains, copied out
// so a timer reads the clocks without holding the Device, which may move.
struct Calibrator {
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetCalibratedTimestampsEXT get = nullptr;  // null: unsupported
  bool host_domain = false;  // `domain` is read beside the device's
  VkTimeDomainEXT domain = VK_TIME_DOMAIN_DEVICE_EXT;
};

// One reading of both clocks, as Device::calibrate documents it.
Result<ClockPair> calibrate(const Calibrator& calibrator);

// The pair one reading makes: the device's ticks; the stamp read in the host
// domain beside them, if any; the driver's deviation; and steady_clock just
// before and after the call. A stamp inside [before, after] is steady_clock's
// own, and keeps the driver's deviation; otherwise the device's reading lies
// somewhere in the call, placed at its middle. The fallback bound adds half
// the call to the driver's uncertainty, saturating on overflow.
// Exported for the tests alone, which reach this header.
VKC_VULKAN_API ClockPair pair_clocks(std::uint64_t device_ticks,
                                     std::optional<std::uint64_t> host_stamp,
                                     std::uint64_t deviation_ns,
                                     std::int64_t before_ns,
                                     std::int64_t after_ns) noexcept;

// The tier's reach into a Device's privates, in one place.
struct DeviceAccess {
  static std::shared_ptr<DeviceRetainedResources> retained(
      const Device& device) {
    return device.retained_;
  }
  static std::shared_ptr<DeviceCounterState> counters(const Device& device) {
    return device.counters_;
  }
  static Calibrator calibrator(const Device& device) noexcept {
    return Calibrator{device.state_.device, device.state_.get_calibrated,
                      device.state_.host_time_domain,
                      device.state_.time_domain};
  }
  static void count_staging(const Device& device) noexcept {
    if (device.counters_ != nullptr) {
      device.counters_->staging_buffers.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // What one command buffer recorded.
  static void count_recorded(const Device& device, std::uint64_t dispatches,
                             std::uint64_t barriers) noexcept {
    if (device.counters_ != nullptr) {
      device.counters_->dispatches.fetch_add(dispatches,
                                             std::memory_order_relaxed);
      device.counters_->barriers.fetch_add(barriers, std::memory_order_relaxed);
    }
  }
};

// Reports `ns` blocked: to the scopes open on this thread, and to `counters`
// (when not null) as one host wait.
void report_blocked(std::int64_t ns, DeviceCounterState* counters) noexcept;

// vkWaitForFences on `fence`, reported as report_blocked says when the fence
// had not signalled already and the timeout is not 0 (a poll, not a wait).
VkResult wait_for_fence(VkDevice device, VkFence fence,
                        std::uint64_t timeout_ns,
                        DeviceCounterState* counters) noexcept;

// The nanoseconds since `start` on the steady clock, for report_blocked.
std::int64_t elapsed_ns(std::chrono::steady_clock::time_point start) noexcept;

}  // namespace volumetric_kit::core::detail
