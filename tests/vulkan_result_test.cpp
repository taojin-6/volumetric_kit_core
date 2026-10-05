// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/vk_result.hpp"

#include <cstdint>
#include <limits>
#include <optional>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

TEST(VkResult, NamesCodes) {
  EXPECT_EQ(to_string(VK_SUCCESS), "VK_SUCCESS");
  EXPECT_EQ(to_string(VK_ERROR_DEVICE_LOST), "VK_ERROR_DEVICE_LOST");
  EXPECT_EQ(to_string(VK_ERROR_OUT_OF_DATE_KHR), "VK_ERROR_OUT_OF_DATE_KHR");
  EXPECT_EQ(to_string(VK_RESULT_MAX_ENUM), "VK_RESULT_UNKNOWN");  // unnamed
}

TEST(VkResult, ErrorIsABackendStatusCarryingTheResult) {
  const Status s = vk_error(VK_ERROR_OUT_OF_DEVICE_MEMORY, "vkAllocateMemory");
  EXPECT_EQ(s.domain(), Status::Code::Backend);
  EXPECT_EQ(s.detail(), VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_EQ(s.message(), "vkAllocateMemory");
  EXPECT_EQ(vk_result(s),
            std::optional<VkResult>(VK_ERROR_OUT_OF_DEVICE_MEMORY));
}

TEST(VkResult, OnlyABackendStatusCarriesAResult) {
  EXPECT_EQ(vk_result(Status{}), std::nullopt);
  EXPECT_EQ(vk_result(Status::unsupported("no")), std::nullopt);
}

// A backend detail outside int32_t, VkResult's range, is no VkResult.
// Converting it to the enum would be undefined, and UBSan does not report the
// conversion: the one this replaced truncated 2^40 to 0, VK_SUCCESS. Each
// bound is checked one past it, and the top one at VK_RESULT_MAX_ENUM
// (INT32_MAX) as well. The cases cast only values VkResult declares, as
// clang-tidy's EnumCastOutOfRange check flags a cast of any other value --
// here or inside vk_result -- so INT32_MIN, in VkResult's range but no
// enumerator, is not checked as kept.
TEST(VkResult, ADetailOutsideInt32CarriesNoResult) {
  constexpr std::int64_t kMin = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
  EXPECT_EQ(vk_result(Status::backend_error(kMax + 1, "wide")), std::nullopt);
  EXPECT_EQ(vk_result(Status::backend_error(kMin - 1, "wide")), std::nullopt);
  EXPECT_EQ(vk_result(Status::backend_error(std::int64_t{1} << 40, "wide")),
            std::nullopt);
  EXPECT_EQ(vk_result(Status::backend_error(
                std::numeric_limits<std::int64_t>::min(), "wide")),
            std::nullopt);
  // A 32-bit code widened without sign extension is outside the range too.
  EXPECT_EQ(vk_result(Status::backend_error(
                static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST), "unsigned")),
            std::nullopt);
  static_assert(VK_RESULT_MAX_ENUM == kMax);
  EXPECT_EQ(vk_result(Status::backend_error(kMax, "narrow")),
            std::optional<VkResult>(VK_RESULT_MAX_ENUM));
}

Status pass_through(VkResult result) {
  VKC_VK_TRY(result);
  return {};
}

TEST(VkResult, TryPassesSuccessAndNamesTheFailingExpression) {
  EXPECT_TRUE(pass_through(VK_SUCCESS).ok());
  const Status s = pass_through(VK_ERROR_DEVICE_LOST);
  EXPECT_EQ(vk_result(s), std::optional<VkResult>(VK_ERROR_DEVICE_LOST));
  EXPECT_EQ(s.message(), "result");
}

}  // namespace
}  // namespace volumetric_kit::core
