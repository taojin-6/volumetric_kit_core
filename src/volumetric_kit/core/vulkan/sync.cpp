// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/sync.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

namespace {

// What a timeline semaphore needs of a device: the feature alone, on any
// queue, at the version the feature itself needs. TimelineSemaphore::create
// checks it, and so does check_timeline_points for a submit that names one.
DeviceRequirements timeline_requirements() {
  DeviceRequirements timeline;
  timeline.api_version = VK_API_VERSION_1_0;
  timeline.queue_flags = 0;
  timeline.timeline_semaphore = true;
  return timeline;
}

}  // namespace

Result<Fence> Fence::create(VkDevice device, bool signaled) {
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("Fence::create: device is null");
  }
  VkFenceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (signaled) info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkFence handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateFence(device, &info, nullptr, &handle));
  Fence fence;
  fence.handle_ = UniqueHandle<VkFence, vkDestroyFence>(device, handle);
  return fence;
}

Status Fence::wait(std::uint64_t timeout_ns) const {
  if (!valid())
    return Status::invalid_argument("Fence::wait on an empty fence");
  VkFence fence = handle_.get();
  const VkResult result =
      vkWaitForFences(handle_.device(), 1, &fence, VK_TRUE, timeout_ns);
  // VK_TIMEOUT too: reported, for the caller to tell apart with vk_result().
  if (result != VK_SUCCESS) return vk_error(result, "vkWaitForFences");
  return {};
}

Status Fence::reset() {
  if (!valid()) {
    return Status::invalid_argument("Fence::reset on an empty fence");
  }
  VkFence fence = handle_.get();
  VKC_VK_TRY(vkResetFences(handle_.device(), 1, &fence));
  return {};
}

bool Fence::is_signaled() const {
  return valid() &&
         vkGetFenceStatus(handle_.device(), handle_.get()) == VK_SUCCESS;
}

Result<Semaphore> Semaphore::create(VkDevice device) {
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("Semaphore::create: device is null");
  }
  VkSemaphoreCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkSemaphore handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateSemaphore(device, &info, nullptr, &handle));
  Semaphore semaphore;
  semaphore.handle_ =
      UniqueHandle<VkSemaphore, vkDestroySemaphore>(device, handle);
  return semaphore;
}

Result<TimelineSemaphore> TimelineSemaphore::create(
    const Device& device, std::uint64_t initial_value) {
  VKC_TRY(device.check_enabled(timeline_requirements())
              .with_context("TimelineSemaphore::create"));
  VkSemaphoreTypeCreateInfo type{};
  type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type.initialValue = initial_value;
  VkSemaphoreCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  info.pNext = &type;
  VkSemaphore handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateSemaphore(device.handle(), &info, nullptr, &handle));
  TimelineSemaphore semaphore;
  semaphore.handle_ =
      UniqueHandle<VkSemaphore, vkDestroySemaphore>(device.handle(), handle);
  return semaphore;
}

TimelineSemaphore::TimelineSemaphore(TimelineSemaphore&& other) noexcept
    : handle_(std::move(other.handle_)),
      submitted_(other.submitted_.exchange(0)) {}

TimelineSemaphore& TimelineSemaphore::operator=(
    TimelineSemaphore&& other) noexcept {
  if (this != &other) {
    handle_ = std::move(other.handle_);
    submitted_ = other.submitted_.exchange(0);
  }
  return *this;
}

Result<std::uint64_t> TimelineSemaphore::value() const {
  if (!valid()) {
    return Status::invalid_argument(
        "TimelineSemaphore::value on an empty semaphore");
  }
  std::uint64_t value = 0;
  VKC_VK_TRY(
      vkGetSemaphoreCounterValue(handle_.device(), handle_.get(), &value));
  return value;
}

Status TimelineSemaphore::signal(std::uint64_t value) {
  // A host signal must advance the counter
  // (VUID-VkSemaphoreSignalInfo-value-03258); refuse one that does not, so a
  // stale value is an error rather than undefined behaviour with layers off.
  // Vulkan has no compare-and-signal, so a signal racing this one from another
  // thread or a queue gets past the check; sync.hpp says how to avoid one.
  VKC_ASSIGN(const std::uint64_t current, this->value());
  if (value <= current) {
    return Status::invalid_argument(
        "TimelineSemaphore::signal: the value must exceed the current one");
  }
  VkSemaphoreSignalInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
  info.semaphore = handle_.get();
  info.value = value;
  VKC_VK_TRY(vkSignalSemaphore(handle_.device(), &info));
  return {};
}

Status TimelineSemaphore::wait(std::uint64_t value,
                               std::uint64_t timeout_ns) const {
  if (!valid()) {
    return Status::invalid_argument(
        "TimelineSemaphore::wait on an empty semaphore");
  }
  VkSemaphore semaphore = handle_.get();
  VkSemaphoreWaitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &semaphore;
  info.pValues = &value;
  const VkResult result = vkWaitSemaphores(handle_.device(), &info, timeout_ns);
  if (result != VK_SUCCESS) return vk_error(result, "vkWaitSemaphores");
  return {};
}

namespace detail {

// TimelineSemaphore's record of the values submits set, read and raised only
// by the two functions below.
struct TimelineSubmits {
  static std::uint64_t highest(const TimelineSemaphore& semaphore) noexcept {
    return semaphore.submitted_.load();
  }
  static void raise(const TimelineSemaphore& semaphore,
                    std::uint64_t value) noexcept {
    std::uint64_t seen = semaphore.submitted_.load();
    while (seen < value &&
           !semaphore.submitted_.compare_exchange_weak(seen, value)) {
    }
  }
};

}  // namespace detail

Status check_timeline_points(const Device& device,
                             const std::vector<TimelinePoint>& wait,
                             const std::vector<TimelinePoint>& signal,
                             const char* call, TimelineWaits waits) {
  if (wait.empty() && signal.empty()) return {};
  const auto refuse = [call](const char* why) {
    return Status::invalid_argument(std::string(call) + ": " + why);
  };
  for (const std::vector<TimelinePoint>* points : {&wait, &signal}) {
    for (const TimelinePoint& point : *points) {
      if (point.semaphore == nullptr || !point.semaphore->valid()) {
        return refuse("a timeline semaphore is null or empty");
      }
    }
  }
  // A device adopted without the feature may share a VkDevice whose
  // semaphores another library made.
  VKC_TRY(device.check_enabled(timeline_requirements()).with_context(call));
  for (const std::vector<TimelinePoint>* points : {&wait, &signal}) {
    for (const TimelinePoint& point : *points) {
      if (point.semaphore->device() != device.handle()) {
        return refuse("a timeline semaphore was made on another VkDevice");
      }
    }
  }
  // The newest value a semaphore is reached or submitted to reach. As for
  // TimelineSemaphore::signal, comparing against it catches a stale value,
  // not a race.
  const auto settled =
      [](const TimelineSemaphore& semaphore) -> Result<std::uint64_t> {
    VKC_ASSIGN(const std::uint64_t current, semaphore.value());
    return std::max(current, detail::TimelineSubmits::highest(semaphore));
  };
  if (waits == TimelineWaits::Submitted) {
    for (const TimelinePoint& point : wait) {
      VKC_ASSIGN(const std::uint64_t reachable, settled(*point.semaphore));
      if (point.value > reachable) {
        return refuse(
            "a value to wait for is neither reached nor set by a submit that "
            "reached a queue");
      }
    }
  }
  // A value to set must advance its counter when the signal runs
  // (VUID-VkSubmitInfo-pSignalSemaphores-03242).
  for (std::size_t i = 0; i < signal.size(); ++i) {
    const TimelineSemaphore& semaphore = *signal[i].semaphore;
    const std::uint64_t value = signal[i].value;
    // One submit's signals run in no set order, so a second value for the
    // same semaphore may go backwards.
    for (std::size_t j = 0; j < i; ++j) {
      if (signal[j].semaphore->handle() == semaphore.handle()) {
        return refuse("a timeline semaphore is set twice in one submit");
      }
    }
    // Set only once every wait is met, so the counter has reached each value
    // waited for on the same semaphore by then.
    for (const TimelinePoint& waited : wait) {
      if (waited.semaphore->handle() == semaphore.handle() &&
          value <= waited.value) {
        return refuse(
            "a value to set must exceed the value the submit waits for on "
            "the same semaphore");
      }
    }
    // An earlier submit's signal, on this queue, runs first.
    VKC_ASSIGN(const std::uint64_t reachable, settled(semaphore));
    if (value <= reachable) {
      return refuse(
          "a value to set must exceed its semaphore's current one, and every "
          "value an earlier submit sets");
    }
  }
  return {};
}

void note_timeline_signals(const std::vector<TimelinePoint>& signal) noexcept {
  for (const TimelinePoint& point : signal) {
    detail::TimelineSubmits::raise(*point.semaphore, point.value);
  }
}

}  // namespace volumetric_kit::core
