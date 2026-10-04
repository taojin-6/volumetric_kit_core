// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// The fixture the vulkan tests share: an instance and the best physical
// device for the test's requirements.
//
// - No device: the test is skipped -- unless VKC_REQUIRE_VULKAN_DEVICE is set
//   (CI sets it beside lavapipe), when it fails, so a runner cannot pass by
//   skipping every GPU test.
// - VKC_TEST_VALIDATION=1: the instance enables the Khronos validation layer,
//   and a validation error, which reaches the log sink with source "vulkan",
//   fails the test.

#include <atomic>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

inline bool env_set(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::string(value) != "0";
}

class VulkanTest : public ::testing::Test {
 protected:
  // The requirements the test's device must meet; the defaults unless a test
  // says otherwise.
  virtual DeviceRequirements requirements() const { return {}; }

  void SetUp() override {
    validation_errors_ = 0;
    set_log_handler([this](LogLevel level, std::string_view source,
                           std::string_view message) {
      if (source == "vulkan" && level == LogLevel::Error) {
        ++validation_errors_;
        ADD_FAILURE() << "validation: " << message;
      }
    });
    InstanceConfig config;
    config.app_name = "volumetric_kit_core tests";
    config.enable_validation = env_set("VKC_TEST_VALIDATION");
    Result<Instance> instance = Instance::create(config);
    if (!instance) {
      no_device("no Vulkan instance: " + instance.status().message());
      return;
    }
    instance_.emplace(*std::move(instance));
    Result<VkPhysicalDevice> physical =
        instance_->select_physical_device(requirements());
    if (!physical) {
      no_device(physical.status().message());
      return;
    }
    physical_ = *physical;
  }

  void TearDown() override {
    set_log_handler({});
    EXPECT_EQ(validation_errors_.load(), 0);
  }

  const Instance& instance() const { return *instance_; }
  VkPhysicalDevice physical() const { return physical_; }

 private:
  void no_device(const std::string& why) {
    if (env_set("VKC_REQUIRE_VULKAN_DEVICE")) {
      FAIL() << "VKC_REQUIRE_VULKAN_DEVICE is set, and " << why;
    }
    GTEST_SKIP() << why;
  }

  std::atomic<int> validation_errors_{0};
  std::optional<Instance> instance_;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
};

}  // namespace volumetric_kit::core::test
