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

// A backend detail wider than 32 bits is no VkResult; converting it to the
// enum would be undefined (UBSan's enum check).
TEST(VkResult, ADetailWiderThan32BitsCarriesNoResult) {
  EXPECT_EQ(vk_result(Status::backend_error(std::int64_t{1} << 40, "wide")),
            std::nullopt);
  EXPECT_EQ(vk_result(Status::backend_error(
                std::numeric_limits<std::int64_t>::min(), "wide")),
            std::nullopt);
  EXPECT_EQ(vk_result(Status::backend_error(
                std::numeric_limits<std::int32_t>::min(), "narrow")),
            std::optional<VkResult>(static_cast<VkResult>(
                std::numeric_limits<std::int32_t>::min())));
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
