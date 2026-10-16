// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/testing/two_family_device.hpp"

#include <memory>
#include <string>
#include <utility>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/testing/write_image.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

TwoFamilyDevice::TwoFamilyDevice(std::unique_ptr<SharedDevice> shared)
    : shared_(std::move(shared)),
      families_{shared_->compute_family(), shared_->graphics_family()} {}

// The members in reverse: the allocators, the adopters, then the shared
// device, which drains both queues.
TwoFamilyDevice::~TwoFamilyDevice() = default;

Result<std::unique_ptr<TwoFamilyDevice>> TwoFamilyDevice::create(
    const DeviceRequirements& compute, const DeviceRequirements& graphics,
    Validation validation) {
  SharedDeviceConfig config;
  config.instance = instance_config(validation);
  config.compute = compute;
  config.graphics = graphics;
  std::unique_ptr<SharedDevice> shared;
  {
    // Read as the instance is created, so they need not outlive it.
    const ValidationSession settings(validation);
    VKC_ASSIGN(shared, SharedDevice::create(config));
  }
  VKC_TRY(check_layer_loaded(shared->instance()));
  std::unique_ptr<TwoFamilyDevice> two(new TwoFamilyDevice(std::move(shared)));
  VkInstance instance = two->shared_->instance().handle();
  VKC_ASSIGN(Device compute_device,
             Device::adopt(two->shared_->compute_payload(), compute));
  two->compute_.emplace(std::move(compute_device));
  VKC_ASSIGN(Device graphics_device,
             Device::adopt(two->shared_->graphics_payload(), graphics));
  two->graphics_.emplace(std::move(graphics_device));
  VKC_ASSIGN(Allocator compute_allocator,
             Allocator::create(instance, two->compute()));
  two->compute_allocator_.emplace(std::move(compute_allocator));
  VKC_ASSIGN(Allocator graphics_allocator,
             Allocator::create(instance, two->graphics()));
  two->graphics_allocator_.emplace(std::move(graphics_allocator));
  return two;
}

// Each set by create before it returns the device.
Device& TwoFamilyDevice::compute() {
  VKC_CHECK(compute_.has_value(), "TwoFamilyDevice::compute() before create");
  return *compute_;
}

Device& TwoFamilyDevice::graphics() {
  VKC_CHECK(graphics_.has_value(), "TwoFamilyDevice::graphics() before create");
  return *graphics_;
}

Allocator& TwoFamilyDevice::compute_allocator() {
  VKC_CHECK(compute_allocator_.has_value(),
            "TwoFamilyDevice::compute_allocator() before create");
  return *compute_allocator_;
}

Allocator& TwoFamilyDevice::graphics_allocator() {
  VKC_CHECK(graphics_allocator_.has_value(),
            "TwoFamilyDevice::graphics_allocator() before create");
  return *graphics_allocator_;
}

Status TwoFamilyDevice::check_shared(VkSharingMode sharing,
                                     const char* call) const {
  if (crosses_families() && sharing != VK_SHARING_MODE_CONCURRENT) {
    return Status::invalid_argument(
        std::string(call) +
        ": the destination is EXCLUSIVE, so a read on the renderer's family "
        "would be undefined, and no layer reports it; name both families() "
        "when making it");
  }
  return {};
}

Status TwoFamilyDevice::write_buffer(const Buffer& dst, VkDeviceSize offset,
                                     const void* data, VkDeviceSize bytes) {
  if (!dst.valid()) {
    return Status::invalid_argument(
        "TwoFamilyDevice::write_buffer: the buffer is empty");
  }
  VKC_TRY(check_shared(dst.sharing_mode(), "TwoFamilyDevice::write_buffer"));
  CommandBatch batch(compute(), compute_allocator());
  VKC_TRY(batch.upload(dst, offset, data, bytes));
  return batch.submit();
}

Status TwoFamilyDevice::write_image(Image& dst, const void* texels,
                                    VkDeviceSize bytes, VkImageLayout layout) {
  // An empty image is write_image's refusal, not a sharing one.
  if (dst.valid()) {
    VKC_TRY(check_shared(dst.sharing_mode(), "TwoFamilyDevice::write_image"));
  }
  return test::write_image(compute(), compute_allocator(), dst, texels, bytes,
                           layout);
}

}  // namespace volumetric_kit::core::test
