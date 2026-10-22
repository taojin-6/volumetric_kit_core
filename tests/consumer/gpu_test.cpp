// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A sibling's GPU test on volumetric_kit::core_test_support: the shared
// device, an allocator, and the environment's policy -- a skip without a
// device, unless VKC_REQUIRE_VULKAN_DEVICE is set -- and the two-family
// device, which also skips on one family.

#include <cstdint>

#include <gtest/gtest.h>

#include "volumetric_kit/core/testing/two_family_device.hpp"
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

class ConsumerCrossFamilyTest : public vkc::test::SharedDeviceTest {};

TEST_F(ConsumerCrossFamilyTest, WritesABufferBothFamiliesShare) {
  vkc::test::TwoFamilyDevice& two = two_family();
  vkc::BufferDesc desc;
  desc.size = 256;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  desc.queue_families = two.families().data();
  desc.queue_family_count = vkc::test::TwoFamilyDevice::kFamilyCount;
  const vkc::Result<vkc::Buffer> buffer =
      two.compute_allocator().create_buffer(desc);
  ASSERT_TRUE(buffer.ok()) << buffer.status().message();
  const std::uint32_t word = 7;
  EXPECT_TRUE(two.write_buffer(*buffer, 0, &word, sizeof(word)).ok());
}

}  // namespace
