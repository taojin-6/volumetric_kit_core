// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A sibling's GPU test that is a main() of its own, on
// volumetric_kit::core_test_policy alone, with no googletest: the
// environment's validation, a skip without a device unless
// VKC_REQUIRE_VULKAN_DEVICE is set, and a failure on the layer's errors.

#include <cstdio>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;

namespace {

int run() {
  vkc::Result<vkc::Instance> instance =
      vkc::Instance::create(test::instance_config());
  if (!instance) {
    return test::no_device_exit_code("no Vulkan instance: " +
                                     instance.status().message());
  }
  if (const vkc::Status loaded = test::check_layer_loaded(*instance);
      !loaded.ok()) {
    std::fprintf(stderr, "%s\n", loaded.message().c_str());
    return 1;
  }
  vkc::Result<vkc::PhysicalDeviceInfo> physical =
      instance->select_physical_device();
  if (!physical) return test::no_device_exit_code(physical.status().message());
  vkc::Result<vkc::Device> device =
      vkc::Device::create(*instance, *physical, {});
  if (!device) {
    std::fprintf(stderr, "%s\n", device.status().message().c_str());
    return 1;
  }
  vkc::Result<vkc::Allocator> allocator =
      vkc::Allocator::create(instance->handle(), *device);
  if (!allocator) {
    std::fprintf(stderr, "%s\n", allocator.status().message().c_str());
    return 1;
  }
  vkc::BufferDesc desc;
  desc.size = 256;
  desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  const vkc::Result<vkc::Buffer> buffer = allocator->create_buffer(desc);
  if (!buffer) {
    std::fprintf(stderr, "%s\n", buffer.status().message().c_str());
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  const test::ValidationSession validation;
  const test::LogCapture log;
  return log.exit_code(run());
}
