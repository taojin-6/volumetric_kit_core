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
  EXPECT_EQ(s.backend(), Status::Backend::Vulkan);
  EXPECT_EQ(s.detail(), VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_EQ(s.message(), "vkAllocateMemory");
  EXPECT_EQ(vk_result(s),
            std::optional<VkResult>(VK_ERROR_OUT_OF_DEVICE_MEMORY));
}

TEST(VkResult, OnlyAVulkanStatusCarriesAResult) {
  EXPECT_EQ(vk_result(Status{}), std::nullopt);
  EXPECT_EQ(vk_result(Status::unsupported("no")), std::nullopt);
  EXPECT_EQ(vk_result(Status::backend_error(Status::Backend::Vulkan,
                                            VK_ERROR_DEVICE_LOST, "vk")),
            std::optional<VkResult>(VK_ERROR_DEVICE_LOST));
}

// Another backend's code is no VkResult, though it is in VkResult's range:
// CUDA_ERROR_OUT_OF_MEMORY and NVJPEG_STATUS_INVALID_PARAMETER are both 2,
// VK_TIMEOUT, and must not read as a wait to retry.
TEST(VkResult, AnotherBackendsStatusCarriesNoResult) {
  for (const Status::Backend backend :
       {Status::Backend::Cuda, Status::Backend::NvJpeg, Status::Backend::Ffmpeg,
        Status::Backend::VideoToolbox, Status::Backend::Other}) {
    for (const VkResult code : {VK_TIMEOUT, VK_ERROR_DEVICE_LOST}) {
      const Status s = Status::backend_error(backend, code, "not Vulkan");
      EXPECT_EQ(vk_result(s), std::nullopt) << to_string(backend);
    }
  }
}

// A Vulkan status with any detail, as vk_error cannot build one.
Status vulkan_code(std::int64_t detail) {
  return Status::backend_error(Status::Backend::Vulkan, detail, "detail");
}

// A Vulkan detail outside int32_t, VkResult's range, is no VkResult.
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
  EXPECT_EQ(vk_result(vulkan_code(kMax + 1)), std::nullopt);
  EXPECT_EQ(vk_result(vulkan_code(kMin - 1)), std::nullopt);
  EXPECT_EQ(vk_result(vulkan_code(std::int64_t{1} << 40)), std::nullopt);
  EXPECT_EQ(vk_result(vulkan_code(std::numeric_limits<std::int64_t>::min())),
            std::nullopt);
  // A 32-bit code widened without sign extension is outside the range too.
  EXPECT_EQ(
      vk_result(vulkan_code(static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST))),
      std::nullopt);
  static_assert(VK_RESULT_MAX_ENUM == kMax);
  EXPECT_EQ(vk_result(vulkan_code(kMax)),
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
