// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Allocator, Buffer and Image on a real device (or lavapipe); see
// vulkan_fixture.hpp for skipping and validation.

#include "volumetric_kit/core/vulkan/allocator.hpp"

#include <cstdint>
#include <cstring>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "vulkan_device_fixture.hpp"

namespace volumetric_kit::core {
namespace {

class AllocatorTest : public test::VulkanDeviceTest {
 protected:
  Buffer make(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory,
              bool mapped = false) {
    BufferDesc desc;
    desc.size = size;
    desc.usage = usage;
    desc.memory = memory;
    desc.mapped = mapped;
    Result<Buffer> buffer = allocator().create_buffer(desc);
    EXPECT_TRUE(buffer.ok()) << buffer.status().message();
    return buffer.ok() ? *std::move(buffer) : Buffer{};
  }
};

constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags kTransfer =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

void transfer_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
}

// --- create
// ----------------------------------------------------------------------

TEST_F(AllocatorTest, RefusesANullInstanceOrAnEmptyDevice) {
  EXPECT_EQ(Allocator::create(VK_NULL_HANDLE, device()).status().domain(),
            Status::Code::InvalidArgument);
  Device moved = std::move(device());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(Allocator::create(instance().handle(), device()).status().domain(),
            Status::Code::InvalidArgument);
  device() = std::move(moved);
}

// --- buffers
// ----------------------------------------------------------------------

TEST_F(AllocatorTest, DeviceLocalBufferLivesInDeviceLocalMemory) {
  const Buffer buffer = make(1 << 16, kStorage, MemoryUsage::DeviceLocal);
  ASSERT_TRUE(buffer.valid());
  EXPECT_EQ(buffer.size(), VkDeviceSize{1 << 16});
  EXPECT_EQ(buffer.usage(), kStorage);
  EXPECT_EQ(buffer.sharing_mode(), VK_SHARING_MODE_EXCLUSIVE);
  EXPECT_EQ(buffer.mapped(), nullptr);
  ASSERT_TRUE(buffer.memory_info().has_value());
  EXPECT_TRUE(buffer.is_device_local());
}

TEST_F(AllocatorTest, HostVisibleBufferIsMappedAndCoherent) {
  const Buffer buffer = make(256, kTransfer, MemoryUsage::HostVisible, true);
  ASSERT_NE(buffer.mapped(), nullptr);
  const std::optional<MemoryInfo> memory = buffer.memory_info();
  // An if, not ASSERT_TRUE: clang-tidy's optional-access check follows the
  // one and not the other.
  if (!memory.has_value()) {
    FAIL() << "no memory info";
  }
  const VkMemoryPropertyFlags props = memory->properties;
  EXPECT_NE(props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0u);
  EXPECT_NE(props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0u);
  auto* bytes = static_cast<std::uint8_t*>(buffer.mapped());
  bytes[0] = 7;
  bytes[255] = 9;
  EXPECT_EQ(bytes[0], 7);
  EXPECT_EQ(bytes[255], 9);
}

TEST_F(AllocatorTest, RefusesContradictoryBuffers) {
  auto domain = [&](const BufferDesc& desc) {
    return allocator().create_buffer(desc).status().domain();
  };
  BufferDesc good;
  good.size = 64;
  good.usage = kStorage;

  BufferDesc zero_size = good;
  zero_size.size = 0;
  EXPECT_EQ(domain(zero_size), Status::Code::InvalidArgument);
  BufferDesc no_usage = good;
  no_usage.usage = 0;
  EXPECT_EQ(domain(no_usage), Status::Code::InvalidArgument);
  BufferDesc mapped_vram = good;
  mapped_vram.memory = MemoryUsage::DeviceLocal;
  mapped_vram.mapped = true;
  EXPECT_EQ(domain(mapped_vram), Status::Code::InvalidArgument);
  BufferDesc unmapped_host = good;
  unmapped_host.memory = MemoryUsage::HostVisible;
  EXPECT_EQ(domain(unmapped_host), Status::Code::InvalidArgument);
}

TEST_F(AllocatorTest, SharingFollowsTheDistinctFamilies) {
  const std::uint32_t family = device().queue_family();
  BufferDesc desc;
  desc.size = 64;
  desc.usage = kStorage;
  desc.memory = MemoryUsage::DeviceLocal;

  const std::uint32_t twice[] = {family, family};
  desc.queue_families = twice;
  desc.queue_family_count = 2;
  const Result<Buffer> same = allocator().create_buffer(desc);
  ASSERT_TRUE(same.ok()) << same.status().message();
  EXPECT_EQ(same->sharing_mode(), VK_SHARING_MODE_EXCLUSIVE);

  const auto families =
      static_cast<std::uint32_t>(device().caps().queue_families().size());
  if (families >= 2) {
    const std::uint32_t both[] = {0, 1};
    desc.queue_families = both;
    const Result<Buffer> shared = allocator().create_buffer(desc);
    ASSERT_TRUE(shared.ok()) << shared.status().message();
    EXPECT_EQ(shared->sharing_mode(), VK_SHARING_MODE_CONCURRENT);
  }

  const std::uint32_t missing[] = {families};  // one past the last family
  desc.queue_families = missing;
  desc.queue_family_count = 1;
  EXPECT_EQ(allocator().create_buffer(desc).status().domain(),
            Status::Code::InvalidArgument);

  const std::uint32_t five[] = {0, 1, 2, 3, 4};
  desc.queue_families = five;
  desc.queue_family_count = 5;
  EXPECT_EQ(allocator().create_buffer(desc).status().domain(),
            Status::Code::InvalidArgument);

  desc.queue_families = nullptr;
  desc.queue_family_count = 1;
  EXPECT_EQ(allocator().create_buffer(desc).status().domain(),
            Status::Code::InvalidArgument);
}

// Upload through a staging buffer into device-local memory and read it back:
// the path every kernel input takes.
TEST_F(AllocatorTest, RoundTripsThroughDeviceLocalMemory) {
  constexpr VkDeviceSize kBytes = 4096;
  const Buffer upload =
      make(kBytes, kTransfer, MemoryUsage::HostVisible, /*mapped=*/true);
  const Buffer resident =
      make(kBytes, kTransfer | kStorage, MemoryUsage::DeviceLocal);
  const Buffer readback =
      make(kBytes, kTransfer, MemoryUsage::HostVisible, /*mapped=*/true);
  ASSERT_TRUE(upload.valid() && resident.valid() && readback.valid());

  std::vector<std::uint8_t> data(kBytes);
  std::iota(data.begin(), data.end(), std::uint8_t{0});
  std::memcpy(upload.mapped(), data.data(), kBytes);
  std::memset(readback.mapped(), 0xAB, kBytes);

  const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
    const VkBufferCopy region{0, 0, kBytes};
    vkCmdCopyBuffer(cmd, upload.handle(), resident.handle(), 1, &region);
    transfer_barrier(cmd);
    vkCmdCopyBuffer(cmd, resident.handle(), readback.handle(), 1, &region);
  });
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(std::memcmp(readback.mapped(), data.data(), kBytes), 0);
}

TEST_F(AllocatorTest, ABufferMayOutliveItsAllocator) {
  Buffer survivor = make(64, kStorage, MemoryUsage::DeviceLocal);
  ASSERT_TRUE(survivor.valid());
  Allocator moved = std::move(allocator());
  EXPECT_TRUE(moved.valid());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(allocator().valid());
  {
    // The allocator goes; the buffer still frees through its VMA state.
    const Allocator gone = std::move(moved);
  }
  EXPECT_TRUE(survivor.valid());
  survivor = Buffer{};
}

TEST_F(AllocatorTest, BuffersMoveAndEmpty) {
  Buffer a = make(64, kStorage, MemoryUsage::DeviceLocal);
  VkBuffer handle = a.handle();
  Buffer b = std::move(a);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_EQ(a.size(), 0u);
  EXPECT_FALSE(a.memory_info().has_value());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(b.handle(), handle);
  b = make(32, kStorage, MemoryUsage::DeviceLocal);  // frees the first
  EXPECT_NE(b.handle(), VK_NULL_HANDLE);
  Buffer* self = &b;
  b = std::move(*self);
  EXPECT_TRUE(b.valid());
}

TEST_F(AllocatorTest, ReportsMemoryPerHeap) {
  const MemoryStats before = allocator().memory_stats();
  ASSERT_GT(before.heap_count, 0u);
  const Buffer buffer = make(1 << 20, kStorage, MemoryUsage::DeviceLocal);
  const std::optional<MemoryInfo> memory = buffer.memory_info();
  if (!memory.has_value()) {
    FAIL() << "no memory info";
  }
  const MemoryStats after = allocator().memory_stats();
  const std::uint32_t heap = memory->heap_index;
  EXPECT_GE(after.heaps[heap].usage_bytes,
            before.heaps[heap].usage_bytes + (1 << 20));
  const Allocator moved = std::move(allocator());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(allocator().memory_stats().heap_count, 0u);
  EXPECT_GT(moved.memory_stats().heap_count, 0u);
}

// --- images
// ---------------------------------------------------------------------

ImageDesc color_image(std::uint32_t width, std::uint32_t height) {
  ImageDesc desc;
  desc.extent = {width, height};
  desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  return desc;
}

TEST_F(AllocatorTest, MakesADeviceLocalImageWithAView) {
  const Result<Image> image = allocator().create_image(color_image(64, 32));
  ASSERT_TRUE(image.ok()) << image.status().message();
  EXPECT_TRUE(image->valid());
  EXPECT_NE(image->view(), VK_NULL_HANDLE);
  EXPECT_EQ(image->width(), 64u);
  EXPECT_EQ(image->height(), 32u);
  EXPECT_EQ(image->depth(), 1u);
  EXPECT_EQ(image->format(), VK_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(image->layout(), VK_IMAGE_LAYOUT_UNDEFINED);
  EXPECT_EQ(image->sharing_mode(), VK_SHARING_MODE_EXCLUSIVE);
  EXPECT_TRUE(image->is_device_local());
}

TEST_F(AllocatorTest, MakesViewlessCubeVolumeAndMippedImages) {
  ImageDesc staging = color_image(16, 16);
  staging.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  staging.with_view = false;
  const Result<Image> viewless = allocator().create_image(staging);
  ASSERT_TRUE(viewless.ok()) << viewless.status().message();
  EXPECT_EQ(viewless->view(), VK_NULL_HANDLE);

  ImageDesc cube = color_image(32, 32);
  cube.cube = true;
  cube.array_layers = 6;
  const Result<Image> cubemap = allocator().create_image(cube);
  ASSERT_TRUE(cubemap.ok()) << cubemap.status().message();
  EXPECT_EQ(cubemap->array_layers(), 6u);
  EXPECT_NE(cubemap->view(), VK_NULL_HANDLE);

  ImageDesc volume;
  volume.type = VK_IMAGE_TYPE_3D;
  volume.extent = {16, 16};
  volume.depth = 8;
  volume.format = VK_FORMAT_R32_SFLOAT;
  volume.usage = VK_IMAGE_USAGE_STORAGE_BIT;
  const Result<Image> voxels = allocator().create_image(volume);
  ASSERT_TRUE(voxels.ok()) << voxels.status().message();
  EXPECT_EQ(voxels->depth(), 8u);

  ImageDesc mipped = color_image(64, 64);
  mipped.mip_levels = 7;
  const Result<Image> chain = allocator().create_image(mipped);
  ASSERT_TRUE(chain.ok()) << chain.status().message();
  EXPECT_EQ(chain->mip_levels(), 7u);

  ImageDesc depth;
  depth.extent = {32, 32};
  depth.format = VK_FORMAT_D32_SFLOAT;
  depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  const Result<Image> z = allocator().create_image(depth);
  ASSERT_TRUE(z.ok()) << z.status().message();
  EXPECT_NE(z->view(), VK_NULL_HANDLE);  // a DEPTH-aspect view
}

TEST_F(AllocatorTest, RefusesContradictoryImages) {
  auto refused = [&](const ImageDesc& desc) {
    return allocator().create_image(desc).status().domain() ==
           Status::Code::InvalidArgument;
  };
  const ImageDesc good = color_image(16, 16);
  ImageDesc d = good;
  d.extent = {0, 16};
  EXPECT_TRUE(refused(d));
  d = good;
  d.usage = 0;
  EXPECT_TRUE(refused(d));
  d = good;
  d.format = VK_FORMAT_UNDEFINED;
  EXPECT_TRUE(refused(d));
  d = good;
  d.depth = 4;  // without a 3D type
  EXPECT_TRUE(refused(d));
  d = good;
  d.type = VK_IMAGE_TYPE_1D;  // with a height of 16
  EXPECT_TRUE(refused(d));
  d = good;
  d.type = VK_IMAGE_TYPE_3D;
  d.array_layers = 2;
  EXPECT_TRUE(refused(d));
  d = good;
  d.cube = true;  // with one layer
  EXPECT_TRUE(refused(d));
  d = good;
  d.samples = VK_SAMPLE_COUNT_4_BIT;
  d.mip_levels = 2;
  EXPECT_TRUE(refused(d));
  d = good;
  d.memory = MemoryUsage::HostVisible;
  EXPECT_TRUE(refused(d));
  d = good;
  d.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // and a view
  EXPECT_TRUE(refused(d));
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = src;
  barrier.dstAccessMask = dst;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
}

// Buffer -> image -> buffer: the image is real, sized and laid out as
// described.
TEST_F(AllocatorTest, RoundTripsThroughAnImage) {
  constexpr std::uint32_t kW = 32;
  constexpr std::uint32_t kH = 8;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kW} * kH * 4;
  const Result<Image> made = allocator().create_image(color_image(kW, kH));
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer upload =
      make(kBytes, kTransfer, MemoryUsage::HostVisible, /*mapped=*/true);
  const Buffer readback =
      make(kBytes, kTransfer, MemoryUsage::HostVisible, /*mapped=*/true);
  std::vector<std::uint8_t> texels(kBytes);
  std::iota(texels.begin(), texels.end(), std::uint8_t{3});
  std::memcpy(upload.mapped(), texels.data(), kBytes);

  VkImage image = made->handle();
  const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = made->extent();
    transition(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, upload.handle(), image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.handle(), 1, &region);
  });
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(std::memcmp(readback.mapped(), texels.data(), kBytes), 0);
}

TEST_F(AllocatorTest, ImagesMoveAndAnAdoptedOneDescribesItself) {
  Result<Image> made = allocator().create_image(color_image(8, 8));
  ASSERT_TRUE(made.ok()) << made.status().message();
  Image a = *std::move(made);
  VkImage handle = a.handle();
  const Image b = std::move(a);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_EQ(a.view(), VK_NULL_HANDLE);
  EXPECT_EQ(a.width(), 0u);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(b.handle(), handle);

  int released = 0;
  {
    ImageInfo info;
    info.image = b.handle();  // describe a borrowed image, freed by the owner
    info.format = b.format();
    info.extent = b.extent();
    info.usage = b.usage();
    info.layout = VK_IMAGE_LAYOUT_GENERAL;
    const Image borrowed(info, [&] { ++released; });
    EXPECT_EQ(borrowed.layout(), VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_FALSE(borrowed.memory_info().has_value());  // nobody said
  }
  EXPECT_EQ(released, 1);
}

}  // namespace
}  // namespace volumetric_kit::core
