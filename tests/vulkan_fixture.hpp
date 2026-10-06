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
//   fails the test -- one reported as the instance is destroyed included. A
//   layer that is missing, fails to load, or cannot reach the log sink fails
//   it too, so a run cannot pass unvalidated.
// - VKC_TEST_SYNC_VALIDATION=1 (with the above): the layer's synchronization
//   validation, enabled through VK_LAYER_ENABLES, must report a deliberate
//   hazard (vulkan_command_batch_test.cpp), so a run cannot pass with it off.

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
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
    allowed_validation_errors_ = 0;
    validation_errors_allowed_ = false;
    {
      const std::scoped_lock lock(warnings_mutex_);
      warnings_.clear();
    }
    set_log_handler([this](LogLevel level, std::string_view source,
                           std::string_view message) {
      const bool validation_error =
          source == "vulkan" && level == LogLevel::Error;
      if (validation_error && validation_errors_allowed_) {
        ++allowed_validation_errors_;
      }
      if (validation_error && !validation_errors_allowed_) {
        ++validation_errors_;
        ADD_FAILURE() << "validation: " << message;
      } else if (level >= LogLevel::Warning) {
        // As the default sink would: why a test fails may be in one.
        std::cerr << '[' << source
                  << (level == LogLevel::Error ? " error] " : " warning] ")
                  << message << '\n';
        if (level == LogLevel::Warning) {
          const std::scoped_lock lock(warnings_mutex_);
          warnings_.emplace_back(message);
        }
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
    if (config.enable_validation && !instance_->validation_logged()) {
      FAIL() << "VKC_TEST_VALIDATION is set, but validation is off or its "
                "messages do not reach the log sink (see the warning above)";
    }
    Result<PhysicalDeviceInfo> physical =
        instance_->select_physical_device(requirements());
    if (!physical) {
      no_device(physical.status().message());
      return;
    }
    physical_ = *std::move(physical);
  }

  void TearDown() override {
    // The instance goes while the handler still counts: the layer reports
    // what outlived it -- a VkDevice never destroyed -- at vkDestroyInstance.
    instance_.reset();
    set_log_handler({});
    EXPECT_EQ(validation_errors_.load(), 0);
  }

  const Instance& instance() const { return *instance_; }
  // The selected device, as select_physical_device captured it.
  const PhysicalDeviceInfo& physical() const { return physical_; }

  // For a test that commits invalid usage on purpose: the validation errors
  // it reports from here on fail nothing, and are counted instead.
  void allow_validation_errors() { validation_errors_allowed_ = true; }
  // The validation errors reported since allow_validation_errors.
  int allowed_validation_errors() const {
    return allowed_validation_errors_.load();
  }
  // The warnings logged since the test began.
  std::vector<std::string> warnings() const {
    const std::scoped_lock lock(warnings_mutex_);
    return warnings_;
  }

 private:
  void no_device(const std::string& why) {
    if (env_set("VKC_REQUIRE_VULKAN_DEVICE")) {
      FAIL() << "VKC_REQUIRE_VULKAN_DEVICE is set, and " << why;
    }
    GTEST_SKIP() << why;
  }

  std::atomic<int> validation_errors_{0};
  std::atomic<int> allowed_validation_errors_{0};
  std::atomic<bool> validation_errors_allowed_{false};
  mutable std::mutex warnings_mutex_;
  std::vector<std::string> warnings_;
  std::optional<Instance> instance_;
  PhysicalDeviceInfo physical_;
};

}  // namespace volumetric_kit::core::test
