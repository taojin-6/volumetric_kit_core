// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "device_access.hpp"
#include "volumetric_kit/core/base/exception_status.hpp"
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

// How the wait on an exported timeline reads its counter (sync.hpp says
// why): kReadNs apart, kSlowReadNs apart once the wait is kSlowAfterNs old,
// so a value long unset costs 1,000 reads a second.
constexpr std::int64_t kReadNs = 50'000;
constexpr std::int64_t kSlowReadNs = 1'000'000;
constexpr std::int64_t kSlowAfterNs = 10'000'000;

// The message of an exported timeline's wait that timed out.
constexpr const char* kCounterShort =
    "TimelineSemaphore::wait: the counter, read until the timeout, is below "
    "the value";

// Reads `semaphore`'s counter, found short at `start`, until it reaches
// `value` (VK_SUCCESS), a read fails (its result), or `timeout_ns` has passed
// since `start` (VK_TIMEOUT, after a last read at the timeout).
VkResult read_until(VkDevice device, VkSemaphore semaphore, std::uint64_t value,
                    std::uint64_t timeout_ns,
                    std::chrono::steady_clock::time_point start) {
  for (;;) {
    const std::int64_t spent = detail::elapsed_ns(start);
    const std::uint64_t used =
        spent > 0 ? static_cast<std::uint64_t>(spent) : 0;
    if (used >= timeout_ns) return VK_TIMEOUT;
    const auto apart = static_cast<std::uint64_t>(
        spent < kSlowAfterNs ? kReadNs : kSlowReadNs);
    // At most kSlowReadNs, so it fits.
    std::this_thread::sleep_for(std::chrono::nanoseconds(
        static_cast<std::int64_t>(std::min(apart, timeout_ns - used))));
    std::uint64_t reached = 0;
    const VkResult read =
        vkGetSemaphoreCounterValue(device, semaphore, &reached);
    if (read != VK_SUCCESS) return read;
    if (reached >= value) return VK_SUCCESS;
  }
}

}  // namespace

namespace detail {

// What TimelineSemaphore::create and create_exported_timeline share: a
// timeline semaphore at `initial_value` on `device`, which enabled
// timelineSemaphore, with `next` -- an export, or nothing -- chained after
// its type.
struct TimelineExport {
  static Result<TimelineSemaphore> make(const Device& device,
                                        std::uint64_t initial_value,
                                        const void* next) {
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.pNext = next;
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
    semaphore.counters_ = DeviceAccess::counters(device);
    semaphore.exported_ = next != nullptr;
    return semaphore;
  }
};

}  // namespace detail

Result<Fence> Fence::create(VkDevice device, bool signaled) try {
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
} catch (...) {
  return exception_status("Fence::create", Status::Code::InvalidArgument);
}

Status Fence::wait(std::uint64_t timeout_ns) const try {
  if (!valid())
    return Status::invalid_argument("Fence::wait on an empty fence");
  // A fence knows no Device, so its waits reach the scopes alone.
  const VkResult result = detail::wait_for_fence(
      handle_.device(), handle_.get(), timeout_ns, nullptr);
  // VK_TIMEOUT too: reported, for the caller to tell apart with is_timeout().
  if (result != VK_SUCCESS) return vk_error(result, "vkWaitForFences");
  return {};
} catch (...) {
  return exception_status("Fence::wait", Status::Code::InvalidArgument);
}

Status Fence::reset() try {
  if (!valid()) {
    return Status::invalid_argument("Fence::reset on an empty fence");
  }
  VkFence fence = handle_.get();
  VKC_VK_TRY(vkResetFences(handle_.device(), 1, &fence));
  return {};
} catch (...) {
  return exception_status("Fence::reset", Status::Code::InvalidArgument);
}

bool Fence::is_signaled() const {
  return valid() &&
         vkGetFenceStatus(handle_.device(), handle_.get()) == VK_SUCCESS;
}

Result<Semaphore> Semaphore::create(VkDevice device) try {
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
} catch (...) {
  return exception_status("Semaphore::create", Status::Code::InvalidArgument);
}

Result<TimelineSemaphore> TimelineSemaphore::create(
    const Device& device, std::uint64_t initial_value) try {
  VKC_TRY(device.check_enabled(timeline_requirements())
              .with_context("TimelineSemaphore::create"));
  return detail::TimelineExport::make(device, initial_value, nullptr);
} catch (...) {
  return exception_status("TimelineSemaphore::create",
                          Status::Code::InvalidArgument);
}

TimelineSemaphore::TimelineSemaphore(TimelineSemaphore&& other) noexcept
    : handle_(std::move(other.handle_)),
      submitted_(other.submitted_.exchange(0)),
      counters_(std::move(other.counters_)),
      exported_(std::exchange(other.exported_, false)) {}

TimelineSemaphore& TimelineSemaphore::operator=(
    TimelineSemaphore&& other) noexcept {
  if (this != &other) {
    handle_ = std::move(other.handle_);
    submitted_ = other.submitted_.exchange(0);
    counters_ = std::move(other.counters_);
    exported_ = std::exchange(other.exported_, false);
  }
  return *this;
}

Result<std::uint64_t> TimelineSemaphore::value() const try {
  if (!valid()) {
    return Status::invalid_argument(
        "TimelineSemaphore::value on an empty semaphore");
  }
  std::uint64_t value = 0;
  VKC_VK_TRY(
      vkGetSemaphoreCounterValue(handle_.device(), handle_.get(), &value));
  return value;
} catch (...) {
  return exception_status("TimelineSemaphore::value",
                          Status::Code::InvalidArgument);
}

Status TimelineSemaphore::signal(std::uint64_t value) try {
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
} catch (...) {
  return exception_status("TimelineSemaphore::signal",
                          Status::Code::InvalidArgument);
}

Status TimelineSemaphore::wait(std::uint64_t value,
                               std::uint64_t timeout_ns) const try {
  if (!valid()) {
    return Status::invalid_argument(
        "TimelineSemaphore::wait on an empty semaphore");
  }
  VkSemaphore semaphore = handle_.get();
  // A poll, or a value already reached, blocks nothing: neither is a wait.
  std::uint64_t reached = 0;
  const VkResult read =
      vkGetSemaphoreCounterValue(handle_.device(), semaphore, &reached);
  const bool done = read == VK_SUCCESS && reached >= value;
  if (exported_) {
    // Another API sets the values, which the driver's wait may see late
    // (sync.hpp): the counter is read until it gets there.
    if (read != VK_SUCCESS) return vk_error(read, "vkGetSemaphoreCounterValue");
    if (done) return {};
    if (timeout_ns == 0) return vk_error(VK_TIMEOUT, kCounterShort);
    const auto start = std::chrono::steady_clock::now();
    const VkResult result =
        read_until(handle_.device(), semaphore, value, timeout_ns, start);
    detail::report_blocked(detail::elapsed_ns(start), counters_.get());
    if (result == VK_TIMEOUT) return vk_error(result, kCounterShort);
    if (result != VK_SUCCESS) {
      return vk_error(result, "vkGetSemaphoreCounterValue");
    }
    return {};
  }
  VkSemaphoreWaitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &semaphore;
  info.pValues = &value;
  const bool waits = timeout_ns != 0 && !done;
  const auto start = std::chrono::steady_clock::now();
  const VkResult result = vkWaitSemaphores(handle_.device(), &info, timeout_ns);
  if (waits) detail::report_blocked(detail::elapsed_ns(start), counters_.get());
  if (result != VK_SUCCESS) return vk_error(result, "vkWaitSemaphores");
  return {};
} catch (...) {
  return exception_status("TimelineSemaphore::wait",
                          Status::Code::InvalidArgument);
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
                             const char* call, TimelineWaits waits) try {
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
  // A submitted signal does not prove its dependencies have been submitted:
  // its producer, or earlier work on its queue, may still wait for the host.
  // TODO: accept unfinished producers only with proof of all their upstream
  // dependencies, including implicit dependencies through queue order.
  if (waits == TimelineWaits::Reached) {
    for (const TimelinePoint& point : wait) {
      VKC_ASSIGN(const std::uint64_t current, point.semaphore->value());
      if (point.value > current) {
        return refuse(
            "a value to wait for has not been reached; a submitted signal "
            "does not prove its upstream dependencies are resolved");
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
    VKC_ASSIGN(const std::uint64_t current, semaphore.value());
    if (value <=
        std::max(current, detail::TimelineSubmits::highest(semaphore))) {
      return refuse(
          "a value to set must exceed its semaphore's current one, and every "
          "value an earlier submit sets");
    }
  }
  return {};
} catch (...) {
  return exception_status("check_timeline_points",
                          Status::Code::InvalidArgument);
}

void note_timeline_signals(const std::vector<TimelinePoint>& signal) noexcept {
  for (const TimelinePoint& point : signal) {
    detail::TimelineSubmits::raise(*point.semaphore, point.value);
  }
}

Result<ExportedTimeline> create_exported_timeline(
    const Device& device, std::uint64_t initial_value) try {
  constexpr const char* kCaller = "create_exported_timeline";
  // A moved-from device first, as TimelineSemaphore::create refuses it, then
  // the feature.
  VKC_TRY(device.check_enabled(timeline_requirements()).with_context(kCaller));
  if (!device.exports_semaphores()) {
    return Status::unsupported(
        "create_exported_timeline: the device does not export semaphores "
        "(VK_KHR_external_semaphore_fd)");
  }
  constexpr VkExternalSemaphoreHandleTypeFlagBits kHandle =
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

  // The extension alone does not promise an exportable timeline: the query
  // names the type, and the semaphore is made only with a handle type the
  // query calls compatible
  // (VUID-VkExportSemaphoreCreateInfo-handleTypes-01124).
  VkSemaphoreTypeCreateInfo timeline{};
  timeline.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  // Zeroed, then set: handleType has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalSemaphoreInfo query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
  query.pNext = &timeline;
  query.handleType = kHandle;
  VkExternalSemaphoreProperties can{};
  can.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
  vkGetPhysicalDeviceExternalSemaphoreProperties(device.physical_device(),
                                                 &query, &can);
  if ((can.externalSemaphoreFeatures &
       VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0 ||
      (can.compatibleHandleTypes & kHandle) == 0) {
    return Status::unsupported(
        "create_exported_timeline: the device cannot export a timeline "
        "semaphore as an opaque file descriptor");
  }

  VkExportSemaphoreCreateInfo exported{};
  exported.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
  exported.handleTypes = kHandle;
  VKC_ASSIGN(TimelineSemaphore semaphore,
             detail::TimelineExport::make(device, initial_value, &exported));
  int fd = -1;
  const VkResult got =
      detail::DeviceAccess::semaphore_fd(device, semaphore.handle(), &fd);
  if (got != VK_SUCCESS) return vk_error(got, "vkGetSemaphoreFdKHR");

  ExportedTimeline out;
  out.semaphore = std::move(semaphore);
  out.fd = UniqueFd(fd);
  return out;
} catch (...) {
  return exception_status("create_exported_timeline",
                          Status::Code::InvalidArgument);
}

}  // namespace volumetric_kit::core
