// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include <cstddef>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "allocation_failure.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

TEST(StatusAllocationTest, ContextFailurePreservesTheOriginalAndCanRetry) {
  const std::string message(160, 'm');
  const std::string context(160, 'c');
  std::string expected = context + ": ";
  expected += message;
  for (const bool persistent : {false, true}) {
    for (std::size_t allowed = 0; allowed < 3; ++allowed) {
      const Status original = Status::invalid_argument(message);
      test::AllocationFailure failure(allowed, persistent);
      const Status result = original.with_context(context);
      const bool failed = failure.stop();
      ASSERT_TRUE(failed);
      EXPECT_EQ(result.domain(), Status::Code::OutOfMemory);
      EXPECT_EQ(original.message(), message);
      const Status retried = original.with_context(context);
      EXPECT_EQ(retried.domain(), Status::Code::InvalidArgument);
      EXPECT_EQ(retried.message(), expected);
    }
  }
}

TEST(StatusAllocationTest, RvalueContextFailurePreservesTheOriginal) {
  const std::string message(160, 'm');
  const std::string context(160, 'c');
  std::string expected = context + ": ";
  expected += message;
  for (const bool persistent : {false, true}) {
    Status original = Status::invalid_argument(message);
    test::AllocationFailure failure(0, persistent);
    const Status result = std::move(original).with_context(context);
    const bool failed = failure.stop();
    ASSERT_TRUE(failed);
    EXPECT_EQ(result.domain(), Status::Code::OutOfMemory);
    // A refused rvalue context operation promises to preserve its input.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_EQ(original.message(), message);
    EXPECT_EQ(original.with_context(context).message(), expected);
  }
}

TEST(StatusAllocationTest, SuccessNeedsNoContextAllocation) {
  const std::string context(160, 'c');
  const Status original;
  test::AllocationFailure failure(0, true);
  const Status result = original.with_context(context);
  const bool failed = failure.stop();
  EXPECT_FALSE(failed);
  EXPECT_TRUE(result.ok());
}

}  // namespace
}  // namespace volumetric_kit::core
