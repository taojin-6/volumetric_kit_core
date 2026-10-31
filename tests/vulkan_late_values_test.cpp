// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Interpose vkQueueSubmit in this executable so a pending submit's fence is
// seen before the timeline values it sets, as on a driver that shows them
// late (lavapipe: Mesa's emulated timelines). While a test holds a gate, a
// submit with a fence and timeline signals goes as two: the work with its
// fence, then its signals, waiting for the gate the host sets. Everything
// else reaches the driver unchanged.

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace {

// Set and read only on the thread that submits: the gate a held submit's
// values wait for, and its value; the fence of the last submit held, and how
// many were.
VkSemaphore held_gate = VK_NULL_HANDLE;
std::uint64_t held_value = 0;
VkFence held_fence = VK_NULL_HANDLE;
unsigned held_submits = 0;

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
              VkFence fence) {
  static const auto submit =
      reinterpret_cast<PFN_vkQueueSubmit>(dlsym(RTLD_NEXT, "vkQueueSubmit"));
  if (submit == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  // Device::submit_pending's shape: one batch, its values first in its chain.
  const auto* values =
      count == 1
          ? static_cast<const VkTimelineSemaphoreSubmitInfo*>(submits[0].pNext)
          : nullptr;
  if (held_gate == VK_NULL_HANDLE || fence == VK_NULL_HANDLE ||
      values == nullptr ||
      values->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO ||
      submits[0].signalSemaphoreCount == 0) {
    return submit(queue, count, submits, fence);
  }
  VkTimelineSemaphoreSubmitInfo work_values = *values;
  work_values.signalSemaphoreValueCount = 0;
  work_values.pSignalSemaphoreValues = nullptr;
  VkSubmitInfo work = submits[0];
  work.pNext = &work_values;
  work.signalSemaphoreCount = 0;
  work.pSignalSemaphores = nullptr;
  const VkResult ran = submit(queue, 1, &work, fence);
  if (ran != VK_SUCCESS) return ran;

  const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkTimelineSemaphoreSubmitInfo late_values{};
  late_values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  late_values.waitSemaphoreValueCount = 1;
  late_values.pWaitSemaphoreValues = &held_value;
  late_values.signalSemaphoreValueCount = values->signalSemaphoreValueCount;
  late_values.pSignalSemaphoreValues = values->pSignalSemaphoreValues;
  VkSubmitInfo late{};
  late.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  late.pNext = &late_values;
  late.waitSemaphoreCount = 1;
  late.pWaitSemaphores = &held_gate;
  late.pWaitDstStageMask = &stage;
  late.signalSemaphoreCount = submits[0].signalSemaphoreCount;
  late.pSignalSemaphores = submits[0].pSignalSemaphores;
  held_fence = fence;
  ++held_submits;
  return submit(queue, 1, &late, VK_NULL_HANDLE);
}

namespace volumetric_kit::core {
namespace {

// Longer than any of these waits should take, so a broken one fails the test
// rather than hanging it.
constexpr std::uint64_t kLongWaitNs = 10'000'000'000;

class LateValuesTest : public test::VulkanDeviceTest {
 protected:
  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    for (TimelineSemaphore* made : {&gate_, &timeline_}) {
      Result<TimelineSemaphore> semaphore = TimelineSemaphore::create(device());
      ASSERT_TRUE(semaphore.ok()) << semaphore.status().message();
      *made = *std::move(semaphore);
    }
  }

  void TearDown() override {
    hold(0);
    held_fence = VK_NULL_HANDLE;
    held_submits = 0;
    gate_ = TimelineSemaphore{};
    timeline_ = TimelineSemaphore{};
    VulkanDeviceTest::TearDown();
  }

  // Holds the values of the next submits until the gate reaches `value`; 0
  // holds none.
  void hold(std::uint64_t value) {
    held_gate = value == 0 ? VK_NULL_HANDLE : gate_.handle();
    held_value = value;
  }

  std::uint64_t counter() const {
    const Result<std::uint64_t> value = timeline_.value();
    EXPECT_TRUE(value.ok()) << value.status().message();
    return value.ok() ? *value : 0;
  }

  // Sets the gate to `value` on scope exit, unless it is there already, so
  // values held for it never outlive a failed assertion. Declared after the
  // pending work, so it runs first.
  class Release {
   public:
    Release(LateValuesTest* test, std::uint64_t value)
        : test_(test), value_(value) {}
    Release(const Release&) = delete;
    Release& operator=(const Release&) = delete;
    Release(Release&&) = delete;
    Release& operator=(Release&&) = delete;
    ~Release() {
      const Result<std::uint64_t> reached = test_->gate_.value();
      if (reached.ok() && *reached < value_) {
        EXPECT_TRUE(test_->gate_.signal(value_).ok());
      }
    }

   private:
    LateValuesTest* test_;
    std::uint64_t value_;
  };

  // Sets the gate to `value` from another thread, 20 ms on, as the host
  // would; joined as it goes out of scope.
  class LateRelease {
   public:
    LateRelease(TimelineSemaphore* gate, std::uint64_t value)
        : thread_([gate, value] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            EXPECT_TRUE(gate->signal(value).ok());
          }) {}
    LateRelease(const LateRelease&) = delete;
    LateRelease& operator=(const LateRelease&) = delete;
    LateRelease(LateRelease&&) = delete;
    LateRelease& operator=(LateRelease&&) = delete;
    ~LateRelease() { thread_.join(); }

   private:
    std::thread thread_;
  };

  TimelineSemaphore gate_;      // only the host sets it
  TimelineSemaphore timeline_;  // only the queue sets it
};

// The fence alone says nothing of the values: a wait returns only once they
// read as set, and a timeout leaves the work pending.
TEST_F(LateValuesTest, AWaitReturnsOnceTheValuesReadAsSet) {
  hold(1);
  Result<Device::PendingSubmit> made =
      device().submit_pending([](VkCommandBuffer) {}, {}, {{&timeline_, 1}});
  ASSERT_TRUE(made.ok()) << made.status().message();
  Device::PendingSubmit pending = *std::move(made);
  const Release release{this, 1};
  ASSERT_EQ(held_submits, 1U) << "the submit was not held";
  ASSERT_EQ(
      vkWaitForFences(device().handle(), 1, &held_fence, VK_TRUE, kLongWaitNs),
      VK_SUCCESS);
  EXPECT_EQ(counter(), 0U);
  EXPECT_TRUE(pending.ready());  // by its fence

  EXPECT_TRUE(is_timeout(pending.wait(20'000'000)));
  EXPECT_TRUE(pending.in_flight());
  ASSERT_TRUE(gate_.signal(1).ok());
  ASSERT_TRUE(pending.wait(kLongWaitNs).ok());
  EXPECT_EQ(counter(), 1U);
  EXPECT_FALSE(pending.in_flight());
}

// Destroying a pending batch, or assigning over it, waits for its values
// too: they read as set the moment either returns, though the host sets the
// gate only later, from another thread.
TEST_F(LateValuesTest, DestroyingOrAssigningOverItWaitsForTheValues) {
  for (const std::uint64_t round : {1U, 2U}) {
    SCOPED_TRACE(round == 1 ? "destroyed" : "assigned over");
    hold(round);
    CommandBatch batch(device(), allocator());
    Result<PendingBatch> made = batch.submit_async({}, {{&timeline_, round}});
    ASSERT_TRUE(made.ok()) << made.status().message();
    const LateRelease release{&gate_, round};
    {
      PendingBatch pending = *std::move(made);
      ASSERT_EQ(held_submits, round) << "the submit was not held";
      ASSERT_EQ(vkWaitForFences(device().handle(), 1, &held_fence, VK_TRUE,
                                kLongWaitNs),
                VK_SUCCESS);
      if (round == 2) {
        pending = PendingBatch{};
        EXPECT_EQ(counter(), round);
      }
    }
    EXPECT_EQ(counter(), round);
  }
}

}  // namespace
}  // namespace volumetric_kit::core
