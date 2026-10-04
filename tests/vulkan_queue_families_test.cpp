// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The queue-family reduction that decides a resource's sharing mode, and the
// config-count check, need no device.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "queue_families.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"

namespace volumetric_kit::core {
namespace {

TEST(QueueFamilies, ReducesToDistinctEntriesInOrder) {
  const std::uint32_t families[] = {2, 0, 2, 0, 1};
  std::uint32_t out[4]{};
  EXPECT_EQ(detail::distinct_queue_families(families, 5, out, 4), 3u);
  EXPECT_EQ(out[0], 2u);
  EXPECT_EQ(out[1], 0u);
  EXPECT_EQ(out[2], 1u);
}

TEST(QueueFamilies, OneFamilyTwiceIsOne) {
  const std::uint32_t families[] = {3, 3};
  std::uint32_t out[4]{};
  EXPECT_EQ(detail::distinct_queue_families(families, 2, out, 4), 1u);
}

TEST(QueueFamilies, ReportsOverflowRatherThanTruncate) {
  const std::uint32_t families[] = {0, 1, 2, 3, 4};
  std::uint32_t out[4]{};
  EXPECT_EQ(detail::distinct_queue_families(families, 5, out, 4), 5u);
}

TEST(QueueFamilies, NoneIsZero) {
  EXPECT_EQ(detail::distinct_queue_families(nullptr, 0, nullptr, 0), 0u);
}

TEST(QueueFamilies, ConfigCountIsBoundedByTheArray) {
  EXPECT_TRUE(check_queue_family_count(0, "test").ok());
  EXPECT_TRUE(
      check_queue_family_count(BufferDesc::kMaxQueueFamilies, "test").ok());
  const Status over =
      check_queue_family_count(BufferDesc::kMaxQueueFamilies + 1, "Caller");
  EXPECT_EQ(over.domain(), Status::Code::InvalidArgument);
  EXPECT_NE(over.message().find("Caller"), std::string::npos);
}

}  // namespace
}  // namespace volumetric_kit::core
