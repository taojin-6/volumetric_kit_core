// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/host_ring.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"

namespace volumetric_kit::core {
namespace {

static_assert(std::is_copy_constructible_v<HostSlot> &&
              std::is_copy_assignable_v<HostSlot>);
static_assert(std::is_nothrow_move_constructible_v<HostSlot> &&
              std::is_nothrow_move_assignable_v<HostSlot>);

constexpr VkDeviceSize kBytes = 64;

void expect_empty(const HostSlot& slot) {
  EXPECT_FALSE(slot.valid());
  EXPECT_EQ(slot.data(), nullptr);
  EXPECT_EQ(slot.capacity(), 0U);
  EXPECT_FALSE(slot.memory_info().has_value());
}

class HostSlotTest : public test::VulkanDeviceTest {};

TEST_F(HostSlotTest, MoveConstructionTransfersTheLeaseAndLeavesAnEmptySource) {
  HostRingDesc desc;
  desc.slot_bytes = kBytes;
  desc.slots = 1;
  Result<std::unique_ptr<HostRing>> made =
      HostRing::create(device(), allocator(), desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  std::unique_ptr<HostRing> ring = *std::move(made);
  Result<HostSlot> claimed = ring->claim(kBytes);
  ASSERT_TRUE(claimed.ok()) << claimed.status().message();
  HostSlot source = *std::move(claimed);
  std::uint8_t* mapping = source.data();
  const VkDeviceSize capacity = source.capacity();
  mapping[0] = 23;

  HostSlot destination = std::move(source);
  // Test the documented moved-from empty state.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  expect_empty(source);
  EXPECT_TRUE(destination.valid());
  EXPECT_EQ(destination.data(), mapping);
  EXPECT_EQ(destination.capacity(), capacity);
  EXPECT_EQ(destination.data()[0], 23);
  EXPECT_EQ(ring->claim(kBytes).status().domain(),
            Status::Code::InvalidArgument);

  destination = HostSlot{};
  expect_empty(destination);
  Result<HostSlot> reclaimed = ring->claim(kBytes);
  ASSERT_TRUE(reclaimed.ok()) << reclaimed.status().message();
  EXPECT_EQ(reclaimed->data(), mapping);
  EXPECT_EQ(reclaimed->data()[0], 23);
}

TEST_F(HostSlotTest, MoveAssignmentReleasesTheOldLeaseAndLeavesAnEmptySource) {
  HostRingDesc desc;
  desc.slot_bytes = kBytes;
  desc.slots = 2;
  Result<std::unique_ptr<HostRing>> made =
      HostRing::create(device(), allocator(), desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  std::unique_ptr<HostRing> ring = *std::move(made);
  Result<HostSlot> first = ring->claim(kBytes);
  Result<HostSlot> second = ring->claim(kBytes);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  HostSlot source = *std::move(first);
  HostSlot destination = *std::move(second);
  std::uint8_t* mapping = source.data();
  std::uint8_t* replaced_mapping = destination.data();
  const VkDeviceSize capacity = source.capacity();
  mapping[0] = 37;
  replaced_mapping[0] = 41;

  destination = std::move(source);
  // Test the documented moved-from empty state.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  expect_empty(source);
  EXPECT_TRUE(destination.valid());
  EXPECT_EQ(destination.data(), mapping);
  EXPECT_EQ(destination.capacity(), capacity);
  EXPECT_EQ(destination.data()[0], 37);
  Result<HostSlot> replaced = ring->claim(kBytes);
  ASSERT_TRUE(replaced.ok()) << replaced.status().message();
  EXPECT_EQ(replaced->data(), replaced_mapping);
  EXPECT_EQ(replaced->data()[0], 41);
  EXPECT_EQ(ring->claim(kBytes).status().domain(),
            Status::Code::InvalidArgument);

  destination = HostSlot{};
  expect_empty(destination);
  Result<HostSlot> reclaimed = ring->claim(kBytes);
  ASSERT_TRUE(reclaimed.ok()) << reclaimed.status().message();
  EXPECT_EQ(reclaimed->data(), mapping);
  EXPECT_EQ(reclaimed->data()[0], 37);
}

}  // namespace
}  // namespace volumetric_kit::core
