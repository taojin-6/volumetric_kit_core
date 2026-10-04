// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// VulkanTest plus a Device and an Allocator on the selected physical device,
// for the tests of the resources made on them.

#include <optional>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "vulkan_fixture.hpp"

namespace volumetric_kit::core::test {

class VulkanDeviceTest : public VulkanTest {
 protected:
  void SetUp() override {
    VulkanTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    Result<Device> device = Device::create(instance(), physical(), {});
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
