// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/testing/write_image.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/format.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {
namespace {

// Whether a color image of `usage` may be left in `layout` (the layer's
// VUID-VkImageMemoryBarrier-oldLayout-* and newLayout rules).
bool leaves_in(VkImageLayout layout, VkImageUsageFlags usage) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_GENERAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return true;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return (usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) != 0;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    default:
      return false;
  }
}

}  // namespace

Status write_image(const Device& device, Allocator& allocator, Image& dst,
                   const void* texels, VkDeviceSize bytes,
                   VkImageLayout layout) {
  const auto refuse = [](const std::string& why) {
    return Status::invalid_argument("test::write_image: " + why);
  };
  if (!dst.valid()) return refuse("the image is empty");
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
  if (!leaves_in(layout, dst.usage())) {
    return refuse("layout " + std::to_string(layout) +
                  " is not GENERAL, TRANSFER_DST_OPTIMAL, or a read or "
                  "attachment layout the image's usage allows");
  }

  BufferDesc desc;
  desc.size = bytes;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  desc.memory = MemoryUsage::Staging;
  VKC_ASSIGN(Buffer made, allocator.create_buffer(desc));
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
    // Every mip and layer, so the one recorded layout holds for all of them.
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                          0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                         1, &b);
  };
  VKC_TRY(device.submit_single_time(
      [&](VkCommandBuffer cmd) {
        // From UNDEFINED, discarding what was there; after whatever touched
        // it before, on any queue, whose work the caller has waited for.
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
        // Visible to whatever this queue runs next; the fence the submit
        // waits for makes the writes available to any other queue.
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, layout);
      },
      staging));
  dst.set_layout(layout);
  return {};
}

}  // namespace volumetric_kit::core::test
