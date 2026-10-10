// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A sibling's GPU test on volumetric_kit::core_test_support: the shared
// device, an allocator, and the environment's policy -- a skip without a
// device, unless VKC_REQUIRE_VULKAN_DEVICE is set.

#include <gtest/gtest.h>

#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"

namespace vkc = volumetric_kit::core;

namespace {

class ConsumerTest : public vkc::test::VulkanDeviceTest {};

TEST_F(ConsumerTest, MakesABufferOnTheSharedDevice) {
  vkc::BufferDesc desc;
  desc.size = 256;
  desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  const vkc::Result<vkc::Buffer> buffer = allocator().create_buffer(desc);
  EXPECT_TRUE(buffer.ok()) << buffer.status().message();
}

}  // namespace
