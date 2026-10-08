// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_support's fixture and the policy under it: what the environment
// asks, the layer's settings, the log capture and exit codes, and a
// validation error or hazard failing the test that commits it. What only a
// process of its own can show -- a missing device or layer, the shared
// device, a leak reported as the process ends -- runs from
// testing_vulkan_fixture_probe.cpp.

#include "volumetric_kit/core/testing/vulkan_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "fill_comp.spv.hpp"
#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/compute_kernel.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

const char* env(const char* name) {
  return std::getenv(name);  // NOLINT(concurrency-mt-unsafe): the test thread
}

// --- the environment ---------------------------------------------------------

TEST(RequestedValidation, FollowsTheEnvironment) {
  const test::ScopedEnv no_validation("VKC_TEST_VALIDATION", nullptr);
  const test::ScopedEnv no_sync("VKC_TEST_SYNC_VALIDATION", nullptr);
  EXPECT_EQ(test::requested_validation(), test::Validation::Off);
  EXPECT_FALSE(test::instance_config().enable_validation);
  // A level of its own, whatever the environment asks.
  EXPECT_TRUE(test::instance_config(test::Validation::Sync).enable_validation);
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

TEST(NoDeviceExitCode, SkipsUnlessADeviceIsRequired) {
  {
    const test::ScopedEnv unset("VKC_REQUIRE_VULKAN_DEVICE", nullptr);
    EXPECT_EQ(test::no_device_exit_code("no device here"), test::kSkipExitCode);
  }
  const test::ScopedEnv required("VKC_REQUIRE_VULKAN_DEVICE", "1");
  EXPECT_EQ(test::no_device_exit_code("no device here"), 1);
}

// --- the layer's settings ----------------------------------------------------

constexpr const char* kValidateSync = "VK_KHRONOS_VALIDATION_VALIDATE_SYNC";
constexpr const char* kLayerEnables = "VK_LAYER_ENABLES";
constexpr const char* kShaderAccesses =
    "VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC";

TEST(ValidationSession, SetsOneSpellingOfSynchronizationValidation) {
  // From a clean slate: the environment's own session may have set them.
  const test::ScopedEnv no_sync(kValidateSync, nullptr);
  const test::ScopedEnv no_enables(kLayerEnables, nullptr);
  const test::ScopedEnv no_shader(kShaderAccesses, nullptr);
  {
    const test::ValidationSession on(test::Validation::On);
    EXPECT_EQ(env(kValidateSync), nullptr);
    EXPECT_EQ(env(kLayerEnables), nullptr);
    EXPECT_EQ(env(kShaderAccesses), nullptr);
  }
  {
    const test::ValidationSession sync(test::Validation::Sync);
    // The setting, or the deprecated spelling on a layer too old for it;
    // never both, which a layer resolves for the deprecated one.
    EXPECT_NE(env(kValidateSync) != nullptr, env(kLayerEnables) != nullptr);
    EXPECT_EQ(env(kShaderAccesses), nullptr);
  }
  {
    const test::ValidationSession shader(test::Validation::ShaderAccesses);
    EXPECT_NE(env(kValidateSync) != nullptr, env(kLayerEnables) != nullptr);
    EXPECT_STREQ(env(kShaderAccesses), "true");
  }
  // Restored on the way out.
  EXPECT_EQ(env(kValidateSync), nullptr);
  EXPECT_EQ(env(kLayerEnables), nullptr);
  EXPECT_EQ(env(kShaderAccesses), nullptr);
}

// --- the log capture ---------------------------------------------------------

TEST(LogCapture, CountsTheLayersErrorsAndKeepsWarnings) {
  std::vector<std::string> handled;
  {
    const test::LogCapture log(
        [&](std::string_view message) { handled.emplace_back(message); });
    log_message(LogLevel::Error, "vulkan", "the layer's error");
    log_message(LogLevel::Error, "app", "an error of another source");
    log_message(LogLevel::Warning, "vulkan", "a warning");
    log_message(LogLevel::Info, "vulkan", "information");
    EXPECT_EQ(log.errors(), 1);
    EXPECT_EQ(log.warnings(), std::vector<std::string>{"a warning"});
    // An error fails a test that passed or skipped, and keeps a failure's.
    EXPECT_EQ(log.exit_code(0), 1);
    EXPECT_EQ(log.exit_code(test::kSkipExitCode), 1);
    EXPECT_EQ(log.exit_code(2), 2);
  }
  EXPECT_EQ(handled, std::vector<std::string>{"the layer's error"});
  const test::LogCapture clean;
  EXPECT_EQ(clean.exit_code(0), 0);
  EXPECT_EQ(clean.exit_code(test::kSkipExitCode), test::kSkipExitCode);
}

TEST(CheckLayerLoaded, FailsOnlyWhereTheEnvironmentAsks) {
  const test::ScopedEnv no_validation("VKC_TEST_VALIDATION", nullptr);
  const test::ScopedEnv no_sync("VKC_TEST_SYNC_VALIDATION", nullptr);
  Result<Instance> instance =
      Instance::create(test::instance_config(test::Validation::Off));
  if (!instance) {
    ASSERT_FALSE(test::device_required()) << instance.status().message();
    GTEST_SKIP() << instance.status().message();
  }
  EXPECT_TRUE(test::check_layer_loaded(*instance).ok());
  const test::ScopedEnv on("VKC_TEST_VALIDATION", "1");
  const Status loaded = test::check_layer_loaded(*instance);
  EXPECT_EQ(loaded.domain(), Status::Code::Unsupported);
  EXPECT_NE(loaded.message().find("validation is off"), std::string::npos);
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

// The first layer with shader-access tracking.
constexpr std::uint32_t kShaderAccessesLayerVersion =
    VK_MAKE_API_VERSION(0, 1, 3, 292);

// Under shader-access tracking wherever the layer has it.
class ShaderAccessesTest : public ValidatedTest {
 protected:
  test::Validation validation() const override {
    return test::Validation::ShaderAccesses;
  }

  void SetUp() override {
    ValidatedTest::SetUp();
    if (base_setup_incomplete()) return;
    if (test::validation_layer_version() < kShaderAccessesLayerVersion) {
      GTEST_SKIP() << "the validation layer predates shader-access tracking";
    }
  }
};

// A compute shader writes a storage buffer, and a copy reads it with no
// barrier between: read after write, which synchronization validation
// reports only when it tracks what the shader accesses.
TEST_F(ShaderAccessesTest, AHazardAgainstAShaderWriteFailsTheTest) {
  constexpr std::uint32_t kCount = 64;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kCount} * 4;
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push.size = 4;
  ComputeKernel fill;
  KernelSetBuilder builder(device());
  ASSERT_TRUE(builder
                  .add(fill, "test_fill", vkc_test_fill_comp_spv,
                       vkc_test_fill_comp_spv_size, 1, &push)
                  .ok());
  const Result<DescriptorPool> pool = builder.build();
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  Result<Buffer> written = device_storage_buffer(allocator(), kBytes);
  Result<Buffer> copied = device_storage_buffer(allocator(), kBytes);
  ASSERT_TRUE(written.ok() && copied.ok());
  fill.set.write_storage_buffer(0, written->handle(), 0, VK_WHOLE_SIZE);
  Status submitted;
  const std::vector<::testing::TestPartResult> failures = failures_of([&] {
    submitted = device().submit_single_time([&](VkCommandBuffer cmd) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        fill.pipeline.handle());
      VkDescriptorSet set = fill.set.handle();
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              fill.pipeline.layout(), 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cmd, fill.pipeline.layout(),
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &kCount);
      vkCmdDispatch(cmd, group_count(kCount, 64), 1, 1);
      const VkBufferCopy region{0, 0, kBytes};
      vkCmdCopyBuffer(cmd, written->handle(), copied->handle(), 1, &region);
    });
  });
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  EXPECT_TRUE(only_vulkan_errors(failures)) << "shader-access tracking is off";
}

}  // namespace
}  // namespace volumetric_kit::core
