// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Probes of core_test_support's fixture that need a process of their own: the
// vkc_testing_* CTest entries (CMakeLists.txt) run these, each with its own
// filter and environment, and read the outcome from the output.

#include <gtest/gtest.h>

#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
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

}  // namespace
}  // namespace volumetric_kit::core
