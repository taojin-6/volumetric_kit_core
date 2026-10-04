// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// DeviceRequirements and merge() need no device, so these run everywhere.

#include "volumetric_kit/core/vulkan/device_requirements.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

using Names = std::vector<std::string>;

TEST(DeviceRequirements, DefaultsAreTheSharedFloor) {
  const DeviceRequirements reqs;
  EXPECT_EQ(reqs.api_version, VK_API_VERSION_1_2);
  EXPECT_EQ(reqs.queue_flags, static_cast<VkQueueFlags>(VK_QUEUE_COMPUTE_BIT));
  EXPECT_TRUE(reqs.timeline_semaphore);
  EXPECT_FALSE(reqs.scalar_block_layout);
  EXPECT_FALSE(reqs.dynamic_rendering);
  EXPECT_FALSE(reqs.needs_present);
}

// A renderer's and a fusion library's needs, as gfx and recon state them.
DeviceRequirements renderer() {
  DeviceRequirements reqs;
  reqs.api_version = VK_API_VERSION_1_3;
  reqs.queue_flags = VK_QUEUE_GRAPHICS_BIT;
  reqs.needs_present = true;
  reqs.dynamic_rendering = true;
  reqs.extensions = {"VK_KHR_external_memory_fd"};
  reqs.features.samplerAnisotropy = VK_TRUE;
  return reqs;
}

DeviceRequirements fusion() {
  DeviceRequirements reqs;
  reqs.scalar_block_layout = true;
  reqs.debug_utils = true;
  reqs.optional_extensions = {"VK_KHR_external_memory_fd",
                              "VK_EXT_metal_objects"};
  reqs.features.shaderInt64 = VK_TRUE;
  return reqs;
}

TEST(DeviceRequirements, MergeTakesTheUnion) {
  const Result<DeviceRequirements> merged = merge(renderer(), fusion());
  ASSERT_TRUE(merged.ok()) << merged.status().message();
  EXPECT_EQ(merged->api_version, VK_API_VERSION_1_3);
  EXPECT_EQ(
      merged->queue_flags,
      static_cast<VkQueueFlags>(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
  EXPECT_TRUE(merged->needs_present);
  EXPECT_TRUE(merged->timeline_semaphore);
  EXPECT_TRUE(merged->scalar_block_layout);
  EXPECT_TRUE(merged->dynamic_rendering);
  EXPECT_TRUE(merged->debug_utils);
  EXPECT_EQ(merged->features.samplerAnisotropy, VK_TRUE);
  EXPECT_EQ(merged->features.shaderInt64, VK_TRUE);
  EXPECT_EQ(merged->features.geometryShader, VK_FALSE);
  // Required by one side, so required; the rest stays optional.
  EXPECT_EQ(merged->extensions, Names{"VK_KHR_external_memory_fd"});
  EXPECT_EQ(merged->optional_extensions, Names{"VK_EXT_metal_objects"});
}

TEST(DeviceRequirements, MergeDeduplicatesInOrder) {
  DeviceRequirements a;
  a.extensions = {"X", "Y"};
  DeviceRequirements b;
  b.extensions = {"Y", "Z"};
  const Result<DeviceRequirements> merged = merge(a, b);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->extensions, (Names{"X", "Y", "Z"}));
}

TEST(DeviceRequirements, MergeKeepsOneFeatureChainAndRefusesTwo) {
  VkPhysicalDeviceVulkan12Features v12{};
  v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceVulkan13Features v13{};
  v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  DeviceRequirements with_chain;
  with_chain.feature_chain = &v12;

  const Result<DeviceRequirements> one = merge(with_chain, {});
  ASSERT_TRUE(one.ok());
  EXPECT_EQ(one->feature_chain, &v12);
  EXPECT_TRUE(merge(with_chain, with_chain).ok());  // the same chain twice

  DeviceRequirements other;
  other.feature_chain = &v13;
  EXPECT_EQ(merge(with_chain, other).status().domain(),
            Status::Code::InvalidArgument);
}

TEST(DeviceRequirements, SupportCheckRefusesAnEmptyDevice) {
  EXPECT_EQ(check_device_support(PhysicalDeviceInfo{}, {}).status().domain(),
            Status::Code::InvalidArgument);
}

}  // namespace
}  // namespace volumetric_kit::core
