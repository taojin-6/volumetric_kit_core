// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A sibling's GPU test that is a main() of its own, on
// volumetric_kit::core_test_policy alone, with no googletest: the
// environment's validation, a skip without a device unless
// VKC_REQUIRE_VULKAN_DEVICE is set, a failure on the layer's errors, an image
// written from texels, and the two-family device.

#include <cstdint>
#include <cstdio>
#include <memory>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/two_family_device.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/testing/write_image.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
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
  vkc::ImageDesc image_desc;
  image_desc.extent = {1, 1};
  image_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_desc.with_view = false;
  vkc::Result<vkc::Image> image = allocator->create_image(image_desc);
  if (!image) {
    std::fprintf(stderr, "%s\n", image.status().message().c_str());
    return 1;
  }
  const std::uint32_t texel = 0xFF00FF00U;
  if (const vkc::Status written =
          test::write_image(*device, *allocator, *image, &texel, sizeof(texel),
                            VK_IMAGE_LAYOUT_GENERAL);
      !written.ok()) {
    std::fprintf(stderr, "%s\n", written.message().c_str());
    return 1;
  }
  vkc::Result<std::unique_ptr<test::TwoFamilyDevice>> two =
      test::TwoFamilyDevice::create({}, {});
  if (!two) {
    std::fprintf(stderr, "%s\n", two.status().message().c_str());
    return 1;
  }
  std::printf("%s: %s\n",
              (*two)->crosses_families() ? "two families" : "one family",
              (*two)->shared().summary().c_str());
  return 0;
}

}  // namespace

int main() {
  const test::ValidationSession validation;
  const test::LogCapture log;
  return log.exit_code(run());
}
