// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Probes of core_test_support's fixture that need a process of their own: the
// vkc_testing_* CTest entries (CMakeLists.txt) run these, each with its own
// filter and environment, and read the outcome from the output.

#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

class Probe : public test::VulkanDeviceTest {};

// The fixture's SetUp alone, which the entries starve of a device or a layer.
TEST_F(Probe, GetsADevice) { EXPECT_NE(device().handle(), VK_NULL_HANDLE); }

// Run in one process, these two share the device: the second destroys the
// fence the first made on it.
VkDevice g_device = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

TEST_F(Probe, MakesAFence) {
  VkFenceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  ASSERT_EQ(vkCreateFence(device().handle(), &info, nullptr, &g_fence),
            VK_SUCCESS);
  g_device = device().handle();
}

TEST_F(Probe, DestroysTheFenceOnTheSameDevice) {
  ASSERT_NE(g_fence, VK_NULL_HANDLE) << "run after Probe.MakesAFence";
  ASSERT_EQ(device().handle(), g_device);
  vkDestroyFence(device().handle(), g_fence, nullptr);
}

// Leaves a buffer on the shared device, which the layer reports as that
// device is destroyed after the last test: the run fails, naming no test.
class LeakProbe : public Probe {
 protected:
  test::Validation validation() const override { return test::Validation::On; }
};

TEST_F(LeakProbe, LeavesABuffer) {
  if (!instance().validation_logged()) {
    GTEST_SKIP() << "the validation layer is not installed, or does not load";
  }
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = 64;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateBuffer(device().handle(), &info, nullptr, &buffer),
            VK_SUCCESS);
}

// A fixture chain whose middle SetUp fails after the base's succeeded: the
// leaf's SetUp returns rather than use what the middle never made.
class MiddleProbe : public Probe {
 protected:
  void SetUp() override {
    Probe::SetUp();
    if (base_setup_incomplete()) return;
    FAIL() << "the middle SetUp stops here";
  }
};

class LeafProbe : public MiddleProbe {
 protected:
  void SetUp() override {
    MiddleProbe::SetUp();
    if (base_setup_incomplete()) return;
    ADD_FAILURE() << "the leaf SetUp carried on";
  }
};

TEST_F(LeafProbe, StopsWhereTheMiddleStopped) {
  ADD_FAILURE() << "the body ran after SetUp failed";
}

// An instance a test makes itself, from test::instance_config(): run under
// VKC_TEST_SYNC_VALIDATION, the environment's settings reach it, it reports
// a write-after-write hazard, and the layer takes the settings without a
// complaint.
TEST(OwnInstanceProbe, RunsTheEnvironmentsSynchronizationValidation) {
  ASSERT_GE(test::requested_validation(), test::Validation::Sync)
      << "run under VKC_TEST_SYNC_VALIDATION=1";
  const test::LogCapture log;
  Result<Instance> instance = Instance::create(test::instance_config());
  if (!instance || !instance->validation_logged()) {
    GTEST_SKIP() << "no instance with the validation layer";
  }
  Result<PhysicalDeviceInfo> physical = instance->select_physical_device();
  if (!physical) GTEST_SKIP() << physical.status().message();
  Result<Device> device = Device::create(*instance, *physical, {});
  ASSERT_TRUE(device.ok()) << device.status().message();
  Result<Allocator> allocator = Allocator::create(instance->handle(), *device);
  ASSERT_TRUE(allocator.ok()) << allocator.status().message();
  BufferDesc desc;
  desc.size = 64;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Result<Buffer> made = allocator->create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer buffer = *std::move(made);
  const int before = log.errors();
  const Status submitted = device->submit_single_time([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 1);
    vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 2);
  });
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  EXPECT_GT(log.errors(), before)
      << "the environment's synchronization validation did not reach an "
         "instance from test::instance_config()";
  for (const std::string& warning : log.warnings()) {
    EXPECT_EQ(warning.find("cannot be mixed"), std::string::npos) << warning;
  }
}

}  // namespace
}  // namespace volumetric_kit::core
