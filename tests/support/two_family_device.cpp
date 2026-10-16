// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/testing/two_family_device.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/format.hpp"
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
  VKC_TRY(check_shared(dst.sharing_mode(), "TwoFamilyDevice::write_buffer"));
  CommandBatch batch(compute(), compute_allocator());
  VKC_TRY(batch.upload(dst, offset, data, bytes));
  return batch.submit();
}

Status TwoFamilyDevice::write_image(Image& dst, const void* texels,
                                    VkDeviceSize bytes, VkImageLayout layout) {
  constexpr const char* kCall = "TwoFamilyDevice::write_image";
  const auto refuse = [&](const std::string& why) {
    return Status::invalid_argument(std::string(kCall) + ": " + why);
  };
  if (!dst.valid()) return refuse("the image is empty");
  VKC_TRY(check_shared(dst.sharing_mode(), kCall));
  const std::uint32_t texel = texel_bytes(dst.format());
  if (texel == 0) {
    return refuse(
        "its format is not an uncompressed single-plane color format");
  }
  if (dst.samples() != VK_SAMPLE_COUNT_1_BIT) {
    return refuse("the image is multisampled");
  }
  if ((dst.usage() & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
    return refuse("the image lacks TRANSFER_DST usage");
  }
  const VkExtent3D extent = dst.extent();
  const VkDeviceSize expected =
      VkDeviceSize{extent.width} * extent.height * extent.depth * texel;
  if (bytes != expected) {
    return refuse("the image takes " + std::to_string(expected) +
                  " bytes, not " + std::to_string(bytes));
  }
  if (texels == nullptr) return refuse("texels is null");
  if (layout == VK_IMAGE_LAYOUT_UNDEFINED ||
      layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
    return refuse("an image cannot be left UNDEFINED or PREINITIALIZED");
  }

  BufferDesc desc;
  desc.size = bytes;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  desc.memory = MemoryUsage::Staging;
  VKC_ASSIGN(Buffer made, compute_allocator().create_buffer(desc));
  auto staging = std::make_shared<Buffer>(std::move(made));
  std::memcpy(staging->mapped(), texels, bytes);

  const auto barrier = [&](VkCommandBuffer cmd, VkPipelineStageFlags src_stage,
                           VkAccessFlags src_access, VkImageLayout from,
                           VkPipelineStageFlags dst_stage,
                           VkAccessFlags dst_access, VkImageLayout to) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst.handle();
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                         1, &b);
  };
  VKC_TRY(compute().submit_single_time(
      [&](VkCommandBuffer cmd) {
        // From UNDEFINED, discarding what was there; after whatever touched
        // it before, on either queue, whose work the caller has waited for.
        barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = extent;
        vkCmdCopyBufferToImage(cmd, staging->handle(), dst.handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        // The fence the submit waits for makes the writes available to the
        // renderer's queue; this barrier orders the transition after them.
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, layout);
      },
      staging));
  dst.set_layout(layout);
  return {};
}

}  // namespace volumetric_kit::core::test
