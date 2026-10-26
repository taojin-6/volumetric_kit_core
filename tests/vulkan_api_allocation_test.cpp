// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include <dlfcn.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocation_failure.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"

namespace {

thread_local bool fail_after_reset = false;
thread_local bool refuse_wait = false;
thread_local bool fail_retention = false;
thread_local std::optional<volumetric_kit::core::test::AllocationFailure>
    after_reset_failure;
thread_local std::optional<volumetric_kit::core::test::AllocationFailure>
    retention_failure;

class ResetFailureGuard {
 public:
  ResetFailureGuard() = default;
  ~ResetFailureGuard() {
    after_reset_failure.reset();
    fail_after_reset = false;
  }
  ResetFailureGuard(const ResetFailureGuard&) = delete;
  ResetFailureGuard& operator=(const ResetFailureGuard&) = delete;
  ResetFailureGuard(ResetFailureGuard&&) = delete;
  ResetFailureGuard& operator=(ResetFailureGuard&&) = delete;
};

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(VkDevice device, std::uint32_t count, const VkFence* fences,
                VkBool32 waitAll, std::uint64_t timeout) {
  static const auto wait = reinterpret_cast<PFN_vkWaitForFences>(
      dlsym(RTLD_NEXT, "vkWaitForFences"));
  if (refuse_wait) {
    if (fail_retention) retention_failure.emplace(0, true);
    return VK_ERROR_UNKNOWN;
  }
  if (wait == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  return wait(device, count, fences, waitAll, timeout);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache,
                       const VkAllocationCallbacks* allocator) {
  static const auto destroy = reinterpret_cast<PFN_vkDestroyPipelineCache>(
      dlsym(RTLD_NEXT, "vkDestroyPipelineCache"));
  // The unconfirmed-command retention happens before this last child is
  // destroyed. Disarm before calling the driver again.
  if (retention_failure) retention_failure->stop();
  if (destroy != nullptr) destroy(device, cache, allocator);
}

// The reset is the last driver call before timed-submit result bookkeeping.
// Arm only after it returns, so neither the driver nor validation consumes
// the injected allocation failure. Static builds alone use this executable.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device,
                                                        std::uint32_t count,
                                                        const VkFence* fences) {
  static const auto reset =
      reinterpret_cast<PFN_vkResetFences>(dlsym(RTLD_NEXT, "vkResetFences"));
  if (reset == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = reset(device, count, fences);
  if (fail_after_reset && result == VK_SUCCESS) {
    fail_after_reset = false;
    after_reset_failure.emplace(0, true);
  }
  return result;
}

namespace volumetric_kit::core {
namespace {

template <class Call>
void expect_diagnostic_failure(const Call& call) {
  for (const bool persistent : {false, true}) {
    test::AllocationFailure failure(0, persistent);
    const Status result = call();
    const bool failed = failure.stop();
    ASSERT_TRUE(failed);
    EXPECT_EQ(result.domain(), Status::Code::OutOfMemory);
  }
}

TEST(ApiAllocationTest, EmptySyncDiagnosticsReturnOutOfMemory) {
  Fence fence;
  TimelineSemaphore semaphore;
  expect_diagnostic_failure(
      [] { return Fence::create(VK_NULL_HANDLE).status(); });
  expect_diagnostic_failure([&] { return fence.wait(0); });
  expect_diagnostic_failure([&] { return fence.reset(); });
  expect_diagnostic_failure(
      [] { return Semaphore::create(VK_NULL_HANDLE).status(); });
  expect_diagnostic_failure([&] { return semaphore.value().status(); });
  expect_diagnostic_failure([&] { return semaphore.signal(1); });
  expect_diagnostic_failure([&] { return semaphore.wait(1, 0); });
}

class DeviceAllocationTest : public test::VulkanDeviceTest {};

TEST_F(DeviceAllocationTest, DeviceValidationDiagnosticsReturnOutOfMemory) {
  auto made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Device kept = *std::move(made);
  // Moved-from validation is a supported public operation.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  const Device& empty = *made;
  expect_diagnostic_failure(
      [&] { return TimelineSemaphore::create(empty).status(); });
  const PhysicalDeviceInfo physical;
  const DeviceRequirements requirements;
  const std::function<void(VkCommandBuffer)> record = [](VkCommandBuffer) {};
  const std::vector<TimelinePoint> points{{nullptr, 1}};
  expect_diagnostic_failure([&] { return empty.check_enabled(requirements); });
  expect_diagnostic_failure([&] { return empty.calibrate().status(); });
  expect_diagnostic_failure(
      [&] { return empty.submit_pending(record, {}, {}).status(); });
  expect_diagnostic_failure(
      [&] { return empty.submit_and_wait(VK_NULL_HANDLE); });
  expect_diagnostic_failure(
      [&] { return Device::adopt({}, requirements).status(); });
  expect_diagnostic_failure(
      [&] { return check_device_support(physical, requirements).status(); });
  expect_diagnostic_failure([&] {
    return check_timeline_points(empty, points, {}, "test",
                                 TimelineWaits::Reached);
  });
}

TEST(ApiAllocationTest, VulkanDiagnosticsRetainTimeoutAndDeviceLoss) {
  const std::string context(160, 'c');
  Status (*volatile convert)(VkResult, std::string_view) = &vk_error;
  for (const VkResult code : {VK_TIMEOUT, VK_ERROR_DEVICE_LOST}) {
    for (const bool persistent : {false, true}) {
      test::AllocationFailure failure(0, persistent);
      const Status result = convert(code, context);
      const bool failed = failure.stop();
      ASSERT_TRUE(failed);
      EXPECT_EQ(vk_result(result), code);
      EXPECT_EQ(is_timeout(result), code == VK_TIMEOUT);
      EXPECT_TRUE(result.message().empty());
    }
  }
}

TEST_F(DeviceAllocationTest,
       RecordingFailureReturnsCommandAndReleasesOwnership) {
  // Throw in user recording after the driver has begun a command; the
  // allocator injector is never armed during a driver or layer call.
  auto lifetime = std::make_shared<int>(1);
  const std::weak_ptr<int> alive = lifetime;
  VkCommandBuffer refused = VK_NULL_HANDLE;
  const std::function<void(VkCommandBuffer)> record =
      [&](VkCommandBuffer command) {
        refused = command;
        throw std::bad_alloc();
      };
  const DeviceCounters before = device().counters();
  const Status blocking = device().submit_single_time(record, lifetime);
  EXPECT_EQ(blocking.domain(), Status::Code::OutOfMemory);
  EXPECT_EQ(device().counters().submits, before.submits);
  const auto pending =
      device().submit_pending(record, {}, {}, std::move(lifetime));
  EXPECT_EQ(pending.status().domain(), Status::Code::OutOfMemory);
  EXPECT_TRUE(alive.expired());
  VkCommandBuffer retried = VK_NULL_HANDLE;
  ASSERT_TRUE(device()
                  .submit_single_time(
                      [&](VkCommandBuffer command) { retried = command; })
                  .ok());
  EXPECT_EQ(retried, refused);
}

TEST_F(DeviceAllocationTest,
       PendingPreparationFailureCanRetryWithoutSubmitting) {
  // A fresh device has no command slots: its two capacity reservations are
  // host work before the first Vulkan fence allocation.
  Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Device& fresh = *made;
  const std::function<void(VkCommandBuffer)> record = [](VkCommandBuffer) {};
  test::AllocationFailure failure(0, true);
  const auto failed_submit = fresh.submit_pending(record, {}, {});
  const bool failed = failure.stop();
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed_submit.status().domain(), Status::Code::OutOfMemory);
  EXPECT_EQ(fresh.counters().submits, 0u);
  auto retried = fresh.submit_pending(record, {}, {});
  ASSERT_TRUE(retried.ok()) << retried.status().message();
  EXPECT_TRUE(retried->wait().ok());
}

TEST_F(DeviceAllocationTest, TimedPreparationFailureDoesNotSubmitAndCanRetry) {
  GpuTimer timer;
  StageMetrics metrics;
  GpuStageScope stage(&metrics, timer, "allocation.test");
  const std::function<void(VkCommandBuffer)> record = [](VkCommandBuffer) {};
  const DeviceCounters before = device().counters();
  // A tile key, its names and serials, and the retained owners are all
  // prepared before recording invokes the driver.
  for (std::size_t allowed = 0; allowed < 5; ++allowed) {
    test::AllocationFailure failure(allowed, true);
    const Status result = device().submit_single_time(record, stage);
    const bool failed = failure.stop();
    ASSERT_TRUE(failed);
    EXPECT_EQ(result.domain(), Status::Code::OutOfMemory);
    EXPECT_EQ(device().counters().submits, before.submits);
  }
  EXPECT_TRUE(device().submit_single_time(record, stage).ok());
}

TEST_F(DeviceAllocationTest, TimingResultAllocationCannotFailCompletedWork) {
  // An unavailable timer still settles host-side timing rows after the
  // command completes, without another driver call.
  GpuTimer timer;
  StageMetrics metrics;
  GpuStageScope stage(&metrics, timer, "allocation.result");
  const DeviceCounters before = device().counters();
  const ResetFailureGuard cleanup;
  fail_after_reset = true;
  const Status result =
      device().submit_single_time([](VkCommandBuffer) {}, stage);
  const bool injected = after_reset_failure && after_reset_failure->stop();
  after_reset_failure.reset();
  fail_after_reset = false;
  ASSERT_TRUE(injected);
  EXPECT_TRUE(result.ok()) << result.message();
  EXPECT_EQ(device().counters().submits, before.submits + 1);
  EXPECT_TRUE(device().submit_single_time([](VkCommandBuffer) {}, stage).ok());
}

TEST_F(DeviceAllocationTest, UnknownCompletionKeepsOwnersDuringPersistentOom) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  // Retention is intentionally permanent when completion is unknown. Run in
  // a child so this safety check leaves no leaked objects in the test runner.
  EXPECT_EXIT(
      {
        auto made = Device::create(instance(), physical(), {});
        if (!made.ok()) std::_Exit(10);
        std::optional<Device> owned(*std::move(made));
        auto lifetime = std::make_shared<int>(1);
        const std::weak_ptr<int> alive = lifetime;
        auto pending = owned->submit_pending([](VkCommandBuffer) {}, {}, {},
                                             std::move(lifetime));
        if (!pending.ok() || !owned->wait_idle().ok()) std::_Exit(11);
        // Real work is complete before interposing unknown completion. The
        // first refusal hands ownership to Device; the destruction refusal
        // arms OOM only after its last driver wait/query has returned.
        refuse_wait = true;
        if (pending->wait().ok()) std::_Exit(12);
        fail_retention = true;
        owned.reset();
        std::_Exit(alive.expired() ? 13 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

}  // namespace
}  // namespace volumetric_kit::core
