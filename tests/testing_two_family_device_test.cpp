// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_support's two-family device and the SharedDeviceTest fixture on
// it: each library on the family SharedDevice carves for it, a buffer and an
// image written on the compute family read back exactly on the renderer's,
// and the writes' refusals. What needs a process of its own -- one device per
// process, a leak, the skip on one family, another family layout -- runs
// from testing_vulkan_fixture_probe.cpp (CMakeLists.txt).

#include "volumetric_kit/core/testing/two_family_device.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

DeviceRequirements graphics_queue() {
  DeviceRequirements graphics;
  graphics.queue_flags = VK_QUEUE_GRAPHICS_BIT;
  return graphics;
}

bool says(const Status& status, const char* what) {
  return status.message().find(what) != std::string::npos;
}

// --- the two-family device, without the fixture -----------------------------

TEST(TwoFamilyDeviceArgs, ARendererThatPresentsIsRefused) {
  DeviceRequirements presenting = graphics_queue();
  presenting.needs_present = true;
  EXPECT_EQ(test::TwoFamilyDevice::create({}, presenting).status().domain(),
            Status::Code::InvalidArgument);
}

// A VulkanTest for the policy: the two-family device's instance logs to the
// same sink, so its layer's errors fail the test too.
using TwoFamilyDeviceTest = test::VulkanTest;

TEST_F(TwoFamilyDeviceTest, CrossesFamiliesExactlyWhereSharedDeviceTakesTwo) {
  Result<std::unique_ptr<test::TwoFamilyDevice>> made =
      test::TwoFamilyDevice::create({}, graphics_queue());
  ASSERT_TRUE(made.ok()) << made.status().message();
  test::TwoFamilyDevice& two = **made;
  const SharedDevice& shared = two.shared();
  const std::string summary = shared.summary();
  EXPECT_EQ(two.crosses_families(), shared.plan() == QueuePlan::TwoFamilies)
      << summary;
  EXPECT_EQ(two.crosses_families(),
            shared.compute_family() != shared.graphics_family())
      << summary;
  const std::array<std::uint32_t, 2> families = {shared.compute_family(),
                                                 shared.graphics_family()};
  EXPECT_EQ(two.families(), families);
  // Each library adopted the one device, on the queue carved for it.
  EXPECT_EQ(two.compute().handle(), shared.device());
  EXPECT_EQ(two.graphics().handle(), shared.device());
  EXPECT_FALSE(two.compute().owns_device());
  EXPECT_EQ(two.compute().queue_family(), shared.compute_family());
  EXPECT_EQ(two.graphics().queue_family(), shared.graphics_family());
  EXPECT_NE(two.compute().queue_flags() & VK_QUEUE_COMPUTE_BIT, 0U);
  EXPECT_NE(two.graphics().queue_flags() & VK_QUEUE_GRAPHICS_BIT, 0U);
  if (two.crosses_families()) {
    EXPECT_NE(two.compute().queue(), two.graphics().queue());
  }
  // At the environment's level, which is the default.
  EXPECT_EQ(shared.instance().validation_logged(),
            test::requested_validation() != test::Validation::Off);
}

// --- the fixture -------------------------------------------------------------

class CrossFamilyTest : public test::SharedDeviceTest {
 protected:
  // A buffer of `bytes` on the compute library's allocator, CONCURRENT
  // across both families unless `exclusive`.
  Result<Buffer> buffer(VkDeviceSize bytes, bool exclusive = false) {
    BufferDesc desc;
    desc.size = bytes;
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!exclusive) {
      desc.queue_families = two_family().families().data();
      desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
    }
    return two_family().compute_allocator().create_buffer(desc);
  }

  // A transfer image of `extent`, as `buffer` makes a buffer.
  Result<Image> image(
      VkExtent2D extent, VkFormat format,
      VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      bool exclusive = false) {
    ImageDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.usage = usage;
    desc.with_view = false;
    if (!exclusive) {
      desc.queue_families = two_family().families().data();
      desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
    }
    return two_family().compute_allocator().create_image(desc);
  }

  // `image`'s texels, copied and read back on the renderer's queue.
  std::vector<std::uint8_t> read_on_graphics(const Image& source,
                                             VkDeviceSize bytes) {
    test::TwoFamilyDevice& two = two_family();
    BufferDesc desc;
    desc.size = bytes;
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Result<Buffer> copied = two.graphics_allocator().create_buffer(desc);
    EXPECT_TRUE(copied.ok()) << copied.status().message();
    std::vector<std::uint8_t> out(bytes, 0);
    if (!copied.ok()) return out;
    CommandBatch read(two.graphics(), two.graphics_allocator());
    EXPECT_TRUE(
        read.copy(source, source.width(), source.height(), *copied, 0).ok());
    EXPECT_TRUE(read.readback(*copied, 0, bytes, out.data()).ok());
    const Status submitted = read.submit();
    EXPECT_TRUE(submitted.ok()) << submitted.message();
    return out;
  }
};

TEST_F(CrossFamilyTest, HandsEachLibraryItsOwnFamily) {
  test::TwoFamilyDevice& two = two_family();
  EXPECT_TRUE(two.crosses_families());
  EXPECT_NE(two.compute().queue_family(), two.graphics().queue_family());
  EXPECT_EQ(two.families()[0], two.compute().queue_family());
  EXPECT_EQ(two.families()[1], two.graphics().queue_family());
  // Validated wherever the layer is installed, as the fixture asks.
  EXPECT_GE(active_validation(), test::Validation::On);
  EXPECT_EQ(two.shared().instance().validation_logged(),
            instance().validation_logged());
}

TEST_F(CrossFamilyTest, ABufferWrittenOnComputeReadsBackExactlyOnGraphics) {
  test::TwoFamilyDevice& two = two_family();
  // Past CommandBatch's inline limit, so the writes are staged.
  constexpr std::uint32_t kWords = 20000;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kWords} * 4;
  constexpr std::uint32_t kHalf = kWords / 2;
  Result<Buffer> made = buffer(kBytes);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer shared = *std::move(made);
  EXPECT_EQ(shared.sharing_mode(), VK_SHARING_MODE_CONCURRENT);

  std::vector<std::uint32_t> first(kWords);
  std::vector<std::uint32_t> second(kHalf);
  for (std::uint32_t i = 0; i < kWords; ++i) first[i] = (i * 2654435761U) + 1;
  for (std::uint32_t i = 0; i < kHalf; ++i) second[i] = ~i;
  ASSERT_TRUE(two.write_buffer(shared, 0, first.data(), kBytes).ok());
  // The second half again, at an offset.
  const Status at = two.write_buffer(shared, VkDeviceSize{kHalf} * 4,
                                     second.data(), VkDeviceSize{kHalf} * 4);
  ASSERT_TRUE(at.ok()) << at.message();

  std::vector<std::uint32_t> read(kWords, 0);
  CommandBatch batch(two.graphics(), two.graphics_allocator());
  ASSERT_TRUE(batch.readback(shared, 0, kBytes, read.data()).ok());
  const Status submitted = batch.submit();
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  std::vector<std::uint32_t> expected(first.begin(), first.begin() + kHalf);
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(read, expected);
}

TEST_F(CrossFamilyTest, AnImageWrittenOnComputeCopiesExactlyOnGraphics) {
  test::TwoFamilyDevice& two = two_family();
  // An odd width, so a row is no power of two in bytes.
  constexpr VkExtent2D kExtent = {37, 23};
  constexpr VkDeviceSize kBytes =
      VkDeviceSize{kExtent.width} * kExtent.height * 4;
  Result<Image> made = image(kExtent, VK_FORMAT_R8G8B8A8_UNORM);
  ASSERT_TRUE(made.ok()) << made.status().message();
  Image shared = *std::move(made);
  EXPECT_EQ(shared.sharing_mode(), VK_SHARING_MODE_CONCURRENT);

  std::vector<std::uint8_t> texels(kBytes);
  for (VkDeviceSize i = 0; i < kBytes; ++i) {
    texels[i] = static_cast<std::uint8_t>((i * 7) + (i / 148));
  }
  ASSERT_TRUE(two.write_image(shared, texels.data(), kBytes,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                  .ok());
  EXPECT_EQ(shared.layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(read_on_graphics(shared, kBytes), texels);

  // Written again after the renderer read it, and left GENERAL.
  for (std::uint8_t& texel : texels) texel = static_cast<std::uint8_t>(~texel);
  ASSERT_TRUE(
      two.write_image(shared, texels.data(), kBytes, VK_IMAGE_LAYOUT_GENERAL)
          .ok());
  EXPECT_EQ(shared.layout(), VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(read_on_graphics(shared, kBytes), texels);
}

TEST_F(CrossFamilyTest, WritesRefuseWhatTheRendererCouldNotRead) {
  test::TwoFamilyDevice& two = two_family();
  const std::vector<std::uint8_t> bytes(64, 1);
  const auto refused = [](const Status& status) {
    return status.domain() == Status::Code::InvalidArgument;
  };

  // An EXCLUSIVE buffer: the renderer's read would be undefined.
  Result<Buffer> exclusive_buffer = buffer(64, /*exclusive=*/true);
  ASSERT_TRUE(exclusive_buffer.ok());
  const Status buffer_refusal =
      two.write_buffer(*exclusive_buffer, 0, bytes.data(), 64);
  EXPECT_TRUE(refused(buffer_refusal) && says(buffer_refusal, "EXCLUSIVE"))
      << buffer_refusal.message();
  // CommandBatch's own refusal comes through.
  Result<Buffer> small = buffer(32);
  ASSERT_TRUE(small.ok());
  EXPECT_TRUE(refused(two.write_buffer(*small, 0, bytes.data(), 64)));

  // Images, each refused before anything is recorded: the layout stays.
  constexpr VkExtent2D kExtent = {4, 4};
  const auto stays = [&](Image& target, VkDeviceSize size, const void* texels,
                         VkImageLayout layout, const char* what) {
    const Status status = two.write_image(target, texels, size, layout);
    EXPECT_TRUE(refused(status) && says(status, what)) << status.message();
    EXPECT_EQ(target.layout(), VK_IMAGE_LAYOUT_UNDEFINED) << what;
  };
  Image empty;
  stays(empty, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "empty");
  Result<Image> exclusive =
      image(kExtent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            /*exclusive=*/true);
  ASSERT_TRUE(exclusive.ok()) << exclusive.status().message();
  stays(*exclusive, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "EXCLUSIVE");
  Result<Image> depth =
      image(kExtent, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ASSERT_TRUE(depth.ok()) << depth.status().message();
  stays(*depth, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "format");
  Result<Image> source_only =
      image(kExtent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  ASSERT_TRUE(source_only.ok()) << source_only.status().message();
  stays(*source_only, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL,
        "TRANSFER_DST");
  Result<Image> good = image(kExtent, VK_FORMAT_R8G8B8A8_UNORM);
  ASSERT_TRUE(good.ok()) << good.status().message();
  stays(*good, 63, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "takes 64 bytes");
  stays(*good, 68, bytes.data(), VK_IMAGE_LAYOUT_GENERAL, "takes 64 bytes");
  stays(*good, 64, nullptr, VK_IMAGE_LAYOUT_GENERAL, "null");
  stays(*good, 64, bytes.data(), VK_IMAGE_LAYOUT_UNDEFINED, "UNDEFINED");
  stays(*good, 64, bytes.data(), VK_IMAGE_LAYOUT_PREINITIALIZED, "UNDEFINED");

  // A multisampled image, where the device can make one to copy into.
  ImageDesc desc;
  desc.extent = kExtent;
  desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  desc.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  desc.samples = VK_SAMPLE_COUNT_4_BIT;
  desc.with_view = false;
  desc.queue_families = two.families().data();
  desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
  VkImageFormatProperties properties{};
  if (vkGetPhysicalDeviceImageFormatProperties(
          two.shared().physical().handle(), desc.format, VK_IMAGE_TYPE_2D,
          VK_IMAGE_TILING_OPTIMAL, desc.usage, 0, &properties) == VK_SUCCESS &&
      (properties.sampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0) {
    Result<Image> multisampled = two.compute_allocator().create_image(desc);
    ASSERT_TRUE(multisampled.ok()) << multisampled.status().message();
    stays(*multisampled, 64, bytes.data(), VK_IMAGE_LAYOUT_GENERAL,
          "multisampled");
  }
}

}  // namespace
}  // namespace volumetric_kit::core
