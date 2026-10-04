// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// VulkanTest plus a Device and an Allocator on the selected physical device,
// for the tests of the resources made on them, and the barrier their readbacks
// share.

#include <optional>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "vulkan_fixture.hpp"

namespace volumetric_kit::core::test {

// Makes the transfers recorded before it visible to the host, for a readback
// through a mapped buffer: a fence wait alone orders the host after the work
// but does not make its writes visible. Coherent memory needs no invalidate
// on top of it.
inline void host_read_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                       0, nullptr);
}

class VulkanDeviceTest : public VulkanTest {
 protected:
  void SetUp() override {
    VulkanTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    // The requirements the device was selected for, so a test that asks for
    // a feature gets it enabled, not just a device that has it.
    Result<Device> device =
        Device::create(instance(), physical(), requirements());
    ASSERT_TRUE(device.ok()) << device.status().message();
    device_.emplace(*std::move(device));
    Result<Allocator> allocator =
        Allocator::create(instance().handle(), *device_);
    ASSERT_TRUE(allocator.ok()) << allocator.status().message();
    allocator_.emplace(*std::move(allocator));
  }

  void TearDown() override {
    allocator_.reset();
    device_.reset();
    VulkanTest::TearDown();
  }

  Device& device() { return *device_; }
  Allocator& allocator() { return *allocator_; }

 private:
  std::optional<Device> device_;
  std::optional<Allocator> allocator_;
};

}  // namespace volumetric_kit::core::test
