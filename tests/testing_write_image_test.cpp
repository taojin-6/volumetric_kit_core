// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_policy's test::write_image on one device: texels read back
// exactly, the whole image left in the layout it records, and its refusals.

#include "volumetric_kit/core/testing/write_image.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

class WriteImageTest : public test::VulkanDeviceTest {
 protected:
  // Barriers: under synchronization validation wherever the layer is.
  test::Validation validation() const override {
    return test::Validation::Sync;
  }

  // A 2D RGBA8 image of `extent`, `mips` and `layers`, with `usage`.
  Result<Image> image(VkExtent2D extent, VkImageUsageFlags usage,
                      std::uint32_t mips = 1, std::uint32_t layers = 1,
                      VkFormat format = VK_FORMAT_R8G8B8A8_UNORM) {
    ImageDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.usage = usage;
    desc.mip_levels = mips;
    desc.array_layers = layers;
    desc.with_view = false;
    return allocator().create_image(desc);
  }

  // Mip 0, layer 0 of `source`, copied out in the layout it records.
  std::vector<std::uint8_t> read(const Image& source, VkDeviceSize bytes) {
    BufferDesc desc;
    desc.size = bytes;
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Result<Buffer> copied = allocator().create_buffer(desc);
    EXPECT_TRUE(copied.ok()) << copied.status().message();
    std::vector<std::uint8_t> out(bytes, 0);
    if (!copied.ok()) return out;
    CommandBatch batch(device(), allocator());
    EXPECT_TRUE(
        batch.copy(source, source.width(), source.height(), *copied, 0).ok());
    EXPECT_TRUE(batch.readback(*copied, 0, bytes, out.data()).ok());
    const Status submitted = batch.submit();
    EXPECT_TRUE(submitted.ok()) << submitted.message();
    return out;
  }
};

std::vector<std::uint8_t> pattern(VkDeviceSize bytes) {
  std::vector<std::uint8_t> texels(bytes);
  for (VkDeviceSize i = 0; i < bytes; ++i) {
    texels[i] = static_cast<std::uint8_t>((i * 13) + (i / 32));
  }
  return texels;
}

TEST_F(WriteImageTest, TexelsReadBackExactlyInEachLayout) {
  constexpr VkExtent2D kExtent = {9, 5};
  constexpr VkDeviceSize kBytes = VkDeviceSize{9} * 5 * 4;
  Result<Image> made = image(kExtent, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ASSERT_TRUE(made.ok()) << made.status().message();
  Image target = *std::move(made);
  std::vector<std::uint8_t> texels = pattern(kBytes);
  for (const VkImageLayout layout :
       {VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}) {
    const Status written = test::write_image(device(), allocator(), target,
                                             texels.data(), kBytes, layout);
    ASSERT_TRUE(written.ok()) << written.message();
    EXPECT_EQ(target.layout(), layout);
    EXPECT_EQ(read(target, kBytes), texels);
    for (std::uint8_t& texel : texels) {
      texel = static_cast<std::uint8_t>(~texel);
    }
  }
}

// Every mip and layer is left in the recorded layout, so a barrier over the
// whole image from it is valid: the layer reports one that is not.
TEST_F(WriteImageTest, LeavesEveryMipAndLayerInTheLayout) {
  constexpr VkExtent2D kExtent = {8, 8};
  constexpr VkDeviceSize kBytes = VkDeviceSize{8} * 8 * 4;
  Result<Image> made =
      image(kExtent,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            /*mips=*/3, /*layers=*/2);
  ASSERT_TRUE(made.ok()) << made.status().message();
  Image target = *std::move(made);
  const std::vector<std::uint8_t> texels = pattern(kBytes);
  const Status written =
      test::write_image(device(), allocator(), target, texels.data(), kBytes,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ASSERT_TRUE(written.ok()) << written.message();
  ASSERT_EQ(target.layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  const Status moved = device().submit_single_time([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = target.layout();
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target.handle();
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                          0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  });
  ASSERT_TRUE(moved.ok()) << moved.message();
  target.set_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(read(target, kBytes), texels);
}

TEST_F(WriteImageTest, LeavesAnAttachmentOrADestinationInItsLayout) {
  const std::vector<std::uint8_t> texels(64, 9);
  Result<Image> attachment = image({4, 4}, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ASSERT_TRUE(attachment.ok()) << attachment.status().message();
  for (const VkImageLayout layout : {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}) {
    const Status written = test::write_image(device(), allocator(), *attachment,
                                             texels.data(), 64, layout);
    ASSERT_TRUE(written.ok()) << written.message();
    EXPECT_EQ(attachment->layout(), layout);
  }
}

TEST_F(WriteImageTest, RefusesWhatItCannotWriteOrLeave) {
  const std::vector<std::uint8_t> bytes(64, 1);
  constexpr VkExtent2D kExtent = {4, 4};
  constexpr VkImageUsageFlags kTransfer =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  // Each refused before anything is recorded: the layout stays.
  const auto stays = [&](Image& target, VkDeviceSize size, const void* texels,
                         VkImageLayout layout, const char* what) {
    const Status status =
        test::write_image(device(), allocator(), target, texels, size, layout);
    EXPECT_EQ(status.domain(), Status::Code::InvalidArgument) << what;
    EXPECT_NE(status.message().find(what), std::string::npos)
        << status.message();
    EXPECT_EQ(target.layout(), VK_IMAGE_LAYOUT_UNDEFINED) << what;
  };

  Image empty;
  stays(empty, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "empty");
  Result<Image> depth = image(kExtent, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 1,
                              VK_FORMAT_D32_SFLOAT);
  ASSERT_TRUE(depth.ok()) << depth.status().message();
  stays(*depth, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "format");
  Result<Image> source_only = image(kExtent, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  ASSERT_TRUE(source_only.ok()) << source_only.status().message();
  stays(*source_only, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL,
        "TRANSFER_DST");

  Result<Image> made = image(kExtent, kTransfer);
  ASSERT_TRUE(made.ok()) << made.status().message();
  Image& good = *made;
  stays(good, 63, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "takes 64 bytes");
  stays(good, 68, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "takes 64 bytes");
  stays(good, 64, nullptr, VK_IMAGE_LAYOUT_GENERAL, "null");
  // Layouts no image can be left in, a depth image's or a swapchain's, and
  // ones this image's usage does not allow.
  for (const VkImageLayout layout :
       {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PREINITIALIZED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}) {
    stays(good, 64, bytes.data(), layout, "is not GENERAL");
  }
  Result<Image> destination_only =
      image(kExtent, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ASSERT_TRUE(destination_only.ok()) << destination_only.status().message();
  stays(*destination_only, 64, bytes.data(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "is not GENERAL");

  // A multisampled image, where the device can make one to copy into.
  ImageDesc desc;
  desc.extent = kExtent;
  desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  desc.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  desc.samples = VK_SAMPLE_COUNT_4_BIT;
  desc.with_view = false;
  VkImageFormatProperties properties{};
  if (vkGetPhysicalDeviceImageFormatProperties(
          physical().handle(), desc.format, VK_IMAGE_TYPE_2D,
          VK_IMAGE_TILING_OPTIMAL, desc.usage, 0, &properties) == VK_SUCCESS &&
      (properties.sampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0) {
    Result<Image> multisampled = allocator().create_image(desc);
    ASSERT_TRUE(multisampled.ok()) << multisampled.status().message();
    stays(*multisampled, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL,
          "multisampled");
  }
}

}  // namespace
}  // namespace volumetric_kit::core
