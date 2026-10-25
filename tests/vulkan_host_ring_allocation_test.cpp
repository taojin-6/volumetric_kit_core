// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "allocation_failure.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/host_ring.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

constexpr VkDeviceSize kBytes = 64;

class HostRingAllocationTest : public test::VulkanDeviceTest {
 protected:
  // No Vulkan call occurs while failure is armed: these operations only
  // retain the lease, append host-side commands, or build diagnostics.
  void recording_failures(bool output, bool persistent) {
    Result<std::unique_ptr<HostRing>> made =
        HostRing::create(device(), allocator(), {kBytes, 1, nullptr});
    ASSERT_TRUE(made.ok()) << made.status().message();
    std::unique_ptr<HostRing> ring = *std::move(made);
    const test::RingOutputPoison poison;
    // Every call retains the slot. The copied path also allocates the copy
    // command, and output first allocates its poison-fill command.
    const std::size_t copied_allocations = output ? 3 : 2;
    const std::size_t allocations = ring->in_place() ? 1 : copied_allocations;
    for (std::size_t allowed = 0; allowed < allocations; ++allowed) {
      SCOPED_TRACE(allowed);
      Result<HostSlot> claimed = ring->claim(kBytes);
      ASSERT_TRUE(claimed.ok()) << claimed.status().message();
      HostSlot slot = *std::move(claimed);
      CommandBatch batch(device(), allocator());
      const DeviceCounters before = device().counters();
      test::AllocationFailure failure(allowed, persistent);
      const Result<VkBuffer> result =
          output ? batch.output(slot, 0, kBytes) : batch.input(slot, 0, kBytes);
      const bool failed = failure.stop();
      ASSERT_TRUE(failed);
      ASSERT_FALSE(result.ok());
      EXPECT_EQ(result.status().domain(), Status::Code::OutOfMemory);
      // Both recording entry points and submission preserve the refusal.
      EXPECT_EQ(batch.input(slot, 0, kBytes).status().domain(),
                Status::Code::OutOfMemory);
      EXPECT_EQ(batch.output(slot, 0, kBytes).status().domain(),
                Status::Code::OutOfMemory);
      slot = HostSlot{};
      EXPECT_EQ(batch.submit().domain(), Status::Code::OutOfMemory);
      EXPECT_EQ(device().counters().submits, before.submits);
      // A partially recorded operation must neither submit nor keep a lease
      // past the refused submit.
      EXPECT_TRUE(ring->claim(kBytes).ok());
    }
  }
};

TEST_F(HostRingAllocationTest, CreateContainsItsHostAllocations) {
  // Ring object, slot vector, and two slot control blocks: all allocated
  // before the first buffer placement or driver call.
  for (std::size_t allowed = 0; allowed < 4; ++allowed) {
    SCOPED_TRACE(allowed);
    test::AllocationFailure failure(allowed);
    const Result<std::unique_ptr<HostRing>> made =
        HostRing::create(device(), allocator(), {kBytes, 2, nullptr});
    const bool failed = failure.stop();
    ASSERT_TRUE(failed);
    ASSERT_FALSE(made.ok());
    EXPECT_EQ(made.status().domain(), Status::Code::OutOfMemory);
  }
  EXPECT_TRUE(
      HostRing::create(device(), allocator(), {kBytes, 2, nullptr}).ok());
}

TEST_F(HostRingAllocationTest, FailedLeaseAllocationLeavesTheSlotFree) {
  Result<std::unique_ptr<HostRing>> made =
      HostRing::create(device(), allocator(), {kBytes, 1, nullptr});
  ASSERT_TRUE(made.ok()) << made.status().message();
  Result<HostSlot> initial = (*made)->claim(kBytes);
  ASSERT_TRUE(initial.ok());
  HostSlot slot = *std::move(initial);
  std::uint8_t* const mapping = slot.data();
  mapping[0] = 42;
  slot = HostSlot{};
  const DeviceCounters before = device().counters();
  test::AllocationFailure failure;
  const Result<HostSlot> failed_claim = (*made)->claim(kBytes);
  const bool failed = failure.stop();
  ASSERT_TRUE(failed);
  ASSERT_FALSE(failed_claim.ok());
  EXPECT_EQ(failed_claim.status().domain(), Status::Code::OutOfMemory);
  Result<HostSlot> retried = (*made)->claim(kBytes);
  ASSERT_TRUE(retried.ok()) << retried.status().message();
  EXPECT_EQ(retried->data(), mapping);
  EXPECT_EQ(retried->data()[0], 42);
  EXPECT_EQ(device().counters().staging_buffers, before.staging_buffers);
}

TEST_F(HostRingAllocationTest, RingDiagnosticsContainAllocationFailures) {
  Result<std::unique_ptr<HostRing>> made =
      HostRing::create(device(), allocator(), {kBytes, 1, "test.held.ring"});
  ASSERT_TRUE(made.ok());
  const Result<HostSlot> held = (*made)->claim(kBytes);
  ASSERT_TRUE(held.ok());
  {
    test::AllocationFailure failure;
    const Result<HostSlot> refused = (*made)->claim(kBytes);
    const bool failed = failure.stop();
    EXPECT_TRUE(failed);
    EXPECT_EQ(refused.status().domain(), Status::Code::OutOfMemory);
  }
  {
    test::AllocationFailure failure;
    const Result<HostSlot> refused = (*made)->claim(0);
    const bool failed = failure.stop();
    EXPECT_TRUE(failed);
    EXPECT_EQ(refused.status().domain(), Status::Code::OutOfMemory);
  }
  {
    test::AllocationFailure failure(0, true);
    const Result<std::unique_ptr<HostRing>> refused =
        HostRing::create(device(), allocator(), {});
    const bool failed = failure.stop();
    EXPECT_TRUE(failed);
    EXPECT_EQ(refused.status().domain(), Status::Code::OutOfMemory);
  }
}

TEST_F(HostRingAllocationTest, InputFailuresPoisonTheBatchAndReleaseItsSlot) {
  recording_failures(false, false);
}

TEST_F(HostRingAllocationTest, OutputFailuresPoisonTheBatchAndReleaseItsSlot) {
  recording_failures(true, false);
}

TEST_F(HostRingAllocationTest, PersistentFailuresStillReturnAndPoison) {
  recording_failures(false, true);
  recording_failures(true, true);
}

TEST_F(HostRingAllocationTest, RecordingDiagnosticFailuresPoisonTheBatch) {
  for (const bool output : {false, true}) {
    SCOPED_TRACE(output);
    const HostSlot empty;
    // First count this library's diagnostic allocations, including the copy
    // that saves the first refusal. Small-string capacities differ between
    // standard libraries, so do not hard-code the failing ordinal.
    CommandBatch counted(device(), allocator());
    test::AllocationFailure counter(std::numeric_limits<std::size_t>::max());
    const Result<VkBuffer> invalid = output ? counted.output(empty, 0, kBytes)
                                            : counted.input(empty, 0, kBytes);
    const bool counting_failed = counter.stop();
    ASSERT_FALSE(counting_failed);
    ASSERT_EQ(invalid.status().domain(), Status::Code::InvalidArgument);
    ASSERT_GT(counter.allocations(), 0U);
    for (std::size_t allowed = 0; allowed < counter.allocations(); ++allowed) {
      SCOPED_TRACE(allowed);
      CommandBatch batch(device(), allocator());
      test::AllocationFailure failure(allowed, true);
      const Result<VkBuffer> refused = output ? batch.output(empty, 0, kBytes)
                                              : batch.input(empty, 0, kBytes);
      const bool failed = failure.stop();
      EXPECT_TRUE(failed);
      EXPECT_EQ(refused.status().domain(), Status::Code::OutOfMemory);
      EXPECT_EQ(batch.submit().domain(), Status::Code::OutOfMemory);
    }
  }
}

}  // namespace
}  // namespace volumetric_kit::core
