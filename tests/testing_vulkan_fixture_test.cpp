// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_support's fixture: the policy the environment sets, and a
// validation error failing the test that commits it. What only a process of
// its own can show -- a missing device or layer, the shared device, a leak
// reported as the process ends -- runs from testing_vulkan_fixture_probe.cpp.

#include "volumetric_kit/core/testing/vulkan_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

// --- the environment ---------------------------------------------------------

TEST(RequestedValidation, FollowsTheEnvironment) {
  const test::ScopedEnv no_validation("VKC_TEST_VALIDATION", nullptr);
  const test::ScopedEnv no_sync("VKC_TEST_SYNC_VALIDATION", nullptr);
  EXPECT_EQ(test::requested_validation(), test::Validation::Off);
  EXPECT_FALSE(test::instance_config().enable_validation);
  {
    const test::ScopedEnv on("VKC_TEST_VALIDATION", "1");
    EXPECT_EQ(test::requested_validation(), test::Validation::On);
    EXPECT_TRUE(test::instance_config().enable_validation);
  }
  for (const char* off : {"", "0"}) {
    const test::ScopedEnv validation("VKC_TEST_VALIDATION", off);
    EXPECT_EQ(test::requested_validation(), test::Validation::Off) << off;
  }
  // Synchronization validation brings the layer, and shader-access checks.
  const test::ScopedEnv sync("VKC_TEST_SYNC_VALIDATION", "1");
  EXPECT_EQ(test::requested_validation(), test::Validation::ShaderAccesses);
  EXPECT_TRUE(test::instance_config().enable_validation);
}

TEST(DeviceRequired, FollowsTheEnvironment) {
  const test::ScopedEnv required("VKC_REQUIRE_VULKAN_DEVICE", "1");
  EXPECT_TRUE(test::device_required());
  {
    const test::ScopedEnv unset("VKC_REQUIRE_VULKAN_DEVICE", nullptr);
    EXPECT_FALSE(test::device_required());
  }
  // Restored on the way out.
  EXPECT_TRUE(test::device_required());
  const test::ScopedEnv zero("VKC_REQUIRE_VULKAN_DEVICE", "0");
  EXPECT_FALSE(test::device_required());
}

// --- a validation error fails the test ---------------------------------------

// The failures `statement` reports, on any thread, intercepted instead of
// failing the running test.
template <typename Statement>
std::vector<::testing::TestPartResult> failures_of(Statement&& statement) {
  ::testing::TestPartResultArray results;
  {
    const ::testing::ScopedFakeTestPartResultReporter reporter(
        ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS,
        &results);
    std::forward<Statement>(statement)();
  }
  std::vector<::testing::TestPartResult> failures;
  failures.reserve(static_cast<std::size_t>(results.size()));
  for (int i = 0; i < results.size(); ++i) {
    failures.push_back(results.GetTestPartResult(i));
  }
  return failures;
}

// Whether `failures` holds a "vulkan" error's failure, and nothing else.
::testing::AssertionResult only_vulkan_errors(
    const std::vector<::testing::TestPartResult>& failures) {
  if (failures.empty()) {
    return ::testing::AssertionFailure() << "no failure was reported";
  }
  for (const ::testing::TestPartResult& failure : failures) {
    if (!failure.nonfatally_failed() ||
        std::string(failure.message()).find("[vulkan error]") ==
            std::string::npos) {
      return ::testing::AssertionFailure()
             << "an unexpected result: " << failure.message();
    }
  }
  return ::testing::AssertionSuccess();
}

// Under the validation layer wherever it is installed.
class ValidatedTest : public test::VulkanDeviceTest {
 protected:
  test::Validation validation() const override { return test::Validation::On; }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    if (!instance().validation_logged()) {
      GTEST_SKIP() << "the validation layer is not installed, or does not "
                      "load";
    }
  }
};

// Asks for a zero-sized buffer: invalid usage, which the layer reports and
// stops before it reaches the driver.
void create_empty_buffer(VkDevice device) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = 0;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  (void)vkCreateBuffer(device, &info, nullptr, &buffer);
  if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
}

TEST_F(ValidatedTest, AValidationErrorFailsTheTest) {
  EXPECT_EQ(active_validation(),
            std::max(test::requested_validation(), test::Validation::On));
  EXPECT_TRUE(only_vulkan_errors(
      failures_of([&] { create_empty_buffer(device().handle()); })));
}

TEST_F(ValidatedTest, AnAllowedErrorIsCountedInstead) {
  allow_validation_errors();
  EXPECT_TRUE(
      failures_of([&] { create_empty_buffer(device().handle()); }).empty());
  EXPECT_GT(allowed_validation_errors(), 0);
}

// Under synchronization validation wherever the layer is installed.
class SyncValidatedTest : public ValidatedTest {
 protected:
  test::Validation validation() const override {
    return test::Validation::Sync;
  }
};

// The batch tests' visibility checks mean nothing without synchronization
// validation, so a run that asks for it cannot pass with it off.
TEST_F(SyncValidatedTest, AHazardFailsTheTest) {
  BufferDesc desc;
  desc.size = 64;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Result<Buffer> made = allocator().create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer buffer = *std::move(made);
  Status submitted;
  // Two writes of the same bytes with no barrier between: write after write,
  // which only synchronization validation reports.
  const std::vector<::testing::TestPartResult> failures = failures_of([&] {
    submitted = device().submit_single_time([&](VkCommandBuffer cmd) {
      vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 1);
      vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 2);
    });
  });
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  EXPECT_TRUE(only_vulkan_errors(failures))
      << "synchronization validation is off";
}

}  // namespace
}  // namespace volumetric_kit::core
