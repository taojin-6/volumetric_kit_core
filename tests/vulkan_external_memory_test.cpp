// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// find_memory_type and create_exported_buffer, ported from recon's
// core_external_memory_test, plus a round trip recon could not make without
// CUDA: the exported descriptor imported back into Vulkan, reading the bytes
// written through the export. create_exported_image likewise, its image
// crossing both ways through batches that release it to, and acquire it
// from, outside Vulkan. Export needs VK_KHR_external_memory_fd; where the
// device lacks it, the refusal is what is checked. create_imported_host_buffer
// likewise with VK_EXT_external_memory_host: pages written by the host, as a
// capture card's DMA would, read by a copy on the device. And
// create_exported_timeline with VK_KHR_external_semaphore_fd: its descriptor
// imported into a second device, whose host signals stand in for CUDA's, and
// a batch here held on the device until the importer sets its value.

#include "volumetric_kit/core/vulkan/external_memory.hpp"

#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/testing/write_image.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/format.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"

namespace volumetric_kit::core {
namespace {

constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr VkMemoryPropertyFlags kMapped = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
// The types find_memory_type never chooses, whose features are left off.
constexpr VkMemoryPropertyFlags kSpecial =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT |
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
// An exported buffer's usage, and so its importer's.
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// What `physical` can do with a buffer of kUsage as an opaque descriptor:
// export it, import it.
VkExternalMemoryFeatureFlags opaque_fd_features(VkPhysicalDevice physical) {
  // Zeroed, then set: handleType has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalBufferInfo query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
  query.usage = kUsage;
  query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkExternalBufferProperties can{};
  can.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
  vkGetPhysicalDeviceExternalBufferProperties(physical, &query, &can);
  return can.externalMemoryProperties.externalMemoryFeatures;
}

// An exported image's usage in these tests: a picture plane shaders sample,
// written and read back by copies.
constexpr VkImageUsageFlags kImageUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkExtent2D kPlane{64, 32};

ImageDesc plane_desc(VkFormat format) {
  ImageDesc desc;
  desc.extent = kPlane;
  desc.format = format;
  desc.usage = kImageUsage;
  return desc;
}

// What `physical` can do with an optimal 2D image of `format` and
// kImageUsage as an opaque descriptor, and its largest extent: no features
// where the format takes no such image.
struct ImageExport {
  VkExternalMemoryFeatureFlags features = 0;
  VkExtent3D largest{};
};

ImageExport opaque_fd_image(VkPhysicalDevice physical, VkFormat format) {
  // Zeroed, then set: handleType and type have no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalImageFormatInfo external{};
  external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceImageFormatInfo2 query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  query.pNext = &external;
  query.format = format;
  query.type = VK_IMAGE_TYPE_2D;
  query.tiling = VK_IMAGE_TILING_OPTIMAL;
  query.usage = kImageUsage;
  VkExternalImageFormatProperties external_can{};
  external_can.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 can{};
  can.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  can.pNext = &external_can;
  if (vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &can) !=
      VK_SUCCESS) {
    return {};
  }
  return {external_can.externalMemoryProperties.externalMemoryFeatures,
          can.imageFormatProperties.maxExtent};
}

// `count` bytes of a pattern that differs with `seed`.
std::vector<std::uint8_t> texels(std::size_t count, std::uint32_t seed) {
  std::vector<std::uint8_t> out(count);
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = static_cast<std::uint8_t>((i * seed) + (i >> 8) + seed);
  }
  return out;
}

using MemoryTypeTest = test::VulkanDeviceTest;

TEST_F(MemoryTypeTest, FindsThePlainestTypeThatFits) {
  const VkPhysicalDeviceMemoryProperties& memory =
      physical().memory_properties();
  const auto flags = [&](std::uint32_t i) {
    return memory.memoryTypes[i].propertyFlags;
  };
  const auto general = [&](std::uint32_t i) {
    return (flags(i) & kSpecial) == 0;
  };
  const std::optional<std::uint32_t> local =
      find_memory_type(device(), ~0U, kLocal);
  // An if, not ASSERT_TRUE: clang-tidy's optional-access check follows it.
  if (!local.has_value()) {
    FAIL() << "no device-local memory type";
  }
  EXPECT_NE(flags(*local) & kLocal, 0U);
  EXPECT_TRUE(general(*local));
  for (std::uint32_t i = 0; i < *local; ++i) {
    EXPECT_TRUE((flags(i) & kLocal) == 0 || !general(i)) << "type " << i;
  }
  // A device-local resource stays out of the BAR wherever a general type the
  // host cannot map exists; a device whose only such type is lazily
  // allocated, which is never chosen, has none.
  bool unmapped_local = false;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    unmapped_local =
        unmapped_local ||
        (general(i) && (flags(i) & kLocal) != 0 && (flags(i) & kMapped) == 0);
  }
  if (unmapped_local) {
    EXPECT_EQ(flags(*local) & kMapped, 0U);
  }
  EXPECT_EQ(find_memory_type(device(), ~0U, kLocal, kMapped).has_value(),
            unmapped_local);
  EXPECT_FALSE(find_memory_type(device(), 0, 0).has_value());
  EXPECT_FALSE(
      find_memory_type(device(), 1U << *local, kLocal, kLocal).has_value());
}

TEST_F(MemoryTypeTest, NeverChoosesTypesThatNeedFeaturesLeftOff) {
  for (const VkMemoryPropertyFlags unusable :
       {VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_PROTECTED_BIT},
        VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT},
        VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD},
        VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD}}) {
    EXPECT_FALSE(find_memory_type(device(), ~0U, unusable).has_value());
  }
}

// The fixture's default device asks for no export, so it has none.
TEST_F(MemoryTypeTest, ADeviceWithoutExportRefusesIt) {
  ASSERT_FALSE(device().exports_memory());
  EXPECT_EQ(
      create_exported_buffer(device(), allocator(), 256).status().domain(),
      Status::Code::Unsupported);
  EXPECT_EQ(create_exported_image(device(), allocator(),
                                  plane_desc(VK_FORMAT_R8_UNORM))
                .status()
                .domain(),
            Status::Code::Unsupported);
}

// An allocator of another device is refused before anything is made on
// either: its memory could not be bound to this device's resource. So is a
// moved-from one.
TEST_F(MemoryTypeTest, RefusesAnAllocatorOfAnotherDevice) {
  Result<Device> other = Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other.ok()) << other.status().message();
  Result<Allocator> foreign = Allocator::create(instance().handle(), *other);
  ASSERT_TRUE(foreign.ok()) << foreign.status().message();
  EXPECT_EQ(create_exported_buffer(device(), *foreign, 256).status().domain(),
            Status::Code::InvalidArgument);
  const ImageDesc desc = plane_desc(VK_FORMAT_R8_UNORM);
  EXPECT_EQ(create_exported_image(device(), *foreign, desc).status().domain(),
            Status::Code::InvalidArgument);
  const Allocator taken = *std::move(foreign);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(create_exported_image(device(), *foreign, desc).status().domain(),
            Status::Code::InvalidArgument);
}

// A device asked for export where the driver offers it.
class ExportTest : public test::VulkanDeviceTest {
 protected:
  DeviceRequirements requirements() const override {
    DeviceRequirements r;
    r.optional_extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    return r;
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    // Refused, not failed, wherever export is impossible: without the
    // extension, and with it where the driver exports no such buffer.
    if (!device().exports_memory() ||
        (opaque_fd_features(physical().handle()) &
         VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
      EXPECT_EQ(
          create_exported_buffer(device(), allocator(), 256).status().domain(),
          Status::Code::Unsupported);
      GTEST_SKIP() << "the device cannot export a storage buffer as an opaque "
                      "descriptor";
    }
  }
};

TEST_F(ExportTest, ExportsADeviceOnlyStorageBuffer) {
  EXPECT_EQ(create_exported_buffer(device(), allocator(), 0).status().domain(),
            Status::Code::InvalidArgument);
  constexpr VkDeviceSize kBytes = 4096;
  const MemoryStats before = allocator().memory_stats();
  Result<ExportedBuffer> made =
      create_exported_buffer(device(), allocator(), kBytes);
  ASSERT_TRUE(made.ok()) << made.status().message();
  // Never imported, so its descriptor is still its own, closed with it.
  const ExportedBuffer exported = *std::move(made);
  EXPECT_TRUE(exported.fd.valid());
  EXPECT_GE(exported.memory_size, kBytes);
  EXPECT_EQ(exported.buffer.size(), kBytes);
  EXPECT_NE(exported.buffer.usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0U);
  EXPECT_TRUE(exported.buffer.is_device_local());
  const std::optional<MemoryInfo> memory = exported.buffer.memory_info();
  if (!memory.has_value()) {
    FAIL() << "no memory info";
  }
  const VkPhysicalDeviceMemoryProperties& props =
      physical().memory_properties();
  ASSERT_LT(memory->type_index, props.memoryTypeCount);
  EXPECT_EQ(memory->properties,
            props.memoryTypes[memory->type_index].propertyFlags);
  EXPECT_EQ(memory->heap_index,
            props.memoryTypes[memory->type_index].heapIndex);
  // Placed as DeviceOnly: private wherever the buffer allows it, and never a
  // discrete GPU's BAR window.
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(device().handle(), exported.buffer.handle(),
                                &needs);
  if (find_memory_type(device(), needs.memoryTypeBits, kLocal, kMapped) ||
      !physical().unified_memory()) {
    EXPECT_EQ(memory->properties & kMapped, 0U);
  }
  // Allocated through the allocator, so it holds the memory, its heap's
  // usage counts it, and the allocator's next allocation is budgeted with it.
  const MemoryStats after = allocator().memory_stats();
  ASSERT_LT(memory->heap_index, after.heap_count);
  EXPECT_EQ(
      after.heaps[memory->heap_index].reserved_bytes,
      before.heaps[memory->heap_index].reserved_bytes + exported.memory_size);
  EXPECT_GE(
      after.heaps[memory->heap_index].usage_bytes,
      before.heaps[memory->heap_index].usage_bytes + exported.memory_size);
}

// The descriptor names the exported memory: imported into a second device on
// the same GPU -- another context, as CUDA's is -- a buffer there reads what a
// batch wrote through the export. The import is on another device because an
// opaque descriptor of a dedicated allocation imports on its own device only
// into the very buffer it was made for
// (VUID-VkMemoryDedicatedAllocateInfo-buffer-01879); another device imports
// it into one created alike, as CUDA does.
TEST_F(ExportTest, TheDescriptorImportsTheSameMemory) {
  constexpr std::uint32_t kWords = 1024;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kWords} * 4;
  if ((opaque_fd_features(physical().handle()) &
       VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
    GTEST_SKIP() << "the device exports opaque descriptors but cannot import";
  }

  Result<ExportedBuffer> made =
      create_exported_buffer(device(), allocator(), kBytes);
  ASSERT_TRUE(made.ok()) << made.status().message();
  // Its descriptor is closed with it on any early return, until the import
  // takes it.
  ExportedBuffer exported = *std::move(made);
  const std::optional<MemoryInfo> memory = exported.buffer.memory_info();
  if (!memory.has_value()) {
    FAIL() << "no memory info";
  }
  std::vector<std::uint32_t> words(kWords);
  for (std::uint32_t i = 0; i < kWords; ++i) words[i] = i * 2654435761U;

  // An ordinary storage buffer to a batch, taken over from outside Vulkan and
  // handed back after, for the other device.
  std::vector<std::uint32_t> back(kWords, 0);
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.acquire(exported.buffer, VK_QUEUE_FAMILY_EXTERNAL).ok());
    ASSERT_TRUE(batch.upload(exported.buffer, 0, words.data(), kBytes).ok());
    ASSERT_TRUE(batch.readback(exported.buffer, 0, kBytes, back.data()).ok());
    ASSERT_TRUE(batch.release(exported.buffer, VK_QUEUE_FAMILY_EXTERNAL).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(back, words);

  // The importer: a device of its own, with an allocator for its batch.
  Result<Device> other_made =
      Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other_made.ok()) << other_made.status().message();
  const Device other = *std::move(other_made);
  Result<Allocator> other_allocator_made =
      Allocator::create(instance().handle(), other);
  ASSERT_TRUE(other_allocator_made.ok());
  Allocator other_allocator = *std::move(other_allocator_made);

  // Created as the export's buffer was, and dedicated as its memory was;
  // owned from the start, so no early return leaves either on the other
  // device, and declared memory first, so the buffer goes before it.
  VkDevice vk = other.handle();
  UniqueHandle<VkDeviceMemory, vkFreeMemory> backing;
  VkExternalMemoryBufferCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.pNext = &external;
  info.size = kBytes;
  info.usage = kUsage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer raw_buffer = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateBuffer(vk, &info, nullptr, &raw_buffer), VK_SUCCESS);
  const UniqueHandle<VkBuffer, vkDestroyBuffer> imported(vk, raw_buffer);
  // Zeroed, then set: handleType has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkImportMemoryFdInfoKHR import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  import.fd = exported.fd.get();
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = &import;
  dedicated.buffer = imported.get();
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &dedicated;
  alloc.allocationSize = exported.memory_size;
  alloc.memoryTypeIndex = memory->type_index;
  VkDeviceMemory raw_memory = VK_NULL_HANDLE;
  // A failed import leaves the descriptor with `exported`, which closes it.
  ASSERT_EQ(vkAllocateMemory(vk, &alloc, nullptr, &raw_memory), VK_SUCCESS)
      << "importing the descriptor failed";
  exported.fd.release();  // the import owns it now
  backing = UniqueHandle<VkDeviceMemory, vkFreeMemory>(vk, raw_memory);
  ASSERT_EQ(vkBindBufferMemory(vk, imported.get(), backing.get(), 0),
            VK_SUCCESS);
  // A view for the batch; the owners above free the buffer and memory.
  const Buffer view(imported.get(), kBytes, kUsage, VK_SHARING_MODE_EXCLUSIVE,
                    nullptr, nullptr, memory);
  std::vector<std::uint32_t> through(kWords, 0);
  {
    CommandBatch batch(other, other_allocator);
    ASSERT_TRUE(batch.acquire(view, VK_QUEUE_FAMILY_EXTERNAL).ok());
    ASSERT_TRUE(batch.readback(view, 0, kBytes, through.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(through, words);
}

// A device asked for export, where the driver exports the two planes of a
// 4:2:0 picture, R8 and R8G8, as opaque descriptors.
class ImageExportTest : public test::VulkanDeviceTest {
 protected:
  DeviceRequirements requirements() const override {
    DeviceRequirements r;
    r.optional_extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    return r;
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    // Refused, not failed, wherever export is impossible.
    const auto exports = [&](VkFormat format) {
      return (opaque_fd_image(physical().handle(), format).features &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    };
    if (!device().exports_memory() || !exports(VK_FORMAT_R8_UNORM) ||
        !exports(VK_FORMAT_R8G8_UNORM)) {
      for (const VkFormat format : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM}) {
        if (device().exports_memory() && exports(format)) continue;
        EXPECT_EQ(
            create_exported_image(device(), allocator(), plane_desc(format))
                .status()
                .domain(),
            Status::Code::Unsupported);
      }
      GTEST_SKIP() << "the device cannot export R8 and R8G8 images as opaque "
                      "descriptors";
    }
  }
};

TEST_F(ImageExportTest, ExportsADeviceOnlyImage) {
  for (const VkFormat format : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM}) {
    SCOPED_TRACE(format);
    const MemoryStats before = allocator().memory_stats();
    Result<ExportedImage> made =
        create_exported_image(device(), allocator(), plane_desc(format));
    ASSERT_TRUE(made.ok()) << made.status().message();
    // Never imported, so its descriptor is still its own, closed with it.
    const ExportedImage exported = *std::move(made);
    const Image& image = exported.image;
    EXPECT_TRUE(exported.fd.valid());
    EXPECT_EQ(image.format(), format);
    EXPECT_EQ(image.type(), VK_IMAGE_TYPE_2D);
    EXPECT_EQ(image.width(), kPlane.width);
    EXPECT_EQ(image.height(), kPlane.height);
    EXPECT_EQ(image.depth(), 1U);
    EXPECT_EQ(image.mip_levels(), 1U);
    EXPECT_EQ(image.array_layers(), 1U);
    EXPECT_EQ(image.samples(), VK_SAMPLE_COUNT_1_BIT);
    EXPECT_EQ(image.tiling(), VK_IMAGE_TILING_OPTIMAL);
    EXPECT_EQ(image.usage(), kImageUsage);
    EXPECT_EQ(image.layout(), VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(image.sharing_mode(), VK_SHARING_MODE_EXCLUSIVE);
    EXPECT_NE(image.view(), VK_NULL_HANDLE);
    VkMemoryRequirements needs{};
    vkGetImageMemoryRequirements(device().handle(), image.handle(), &needs);
    EXPECT_GE(exported.memory_size, needs.size);
    const std::optional<MemoryInfo> memory = image.memory_info();
    if (!memory.has_value()) {
      FAIL() << "no memory info";
    }
    EXPECT_TRUE(image.is_device_local());
    // Placed as DeviceOnly, as an exported buffer is.
    if (find_memory_type(device(), needs.memoryTypeBits, kLocal, kMapped) ||
        !physical().unified_memory()) {
      EXPECT_EQ(memory->properties & kMapped, 0U);
    }
    // Its own memory, through the allocator, so its heap counts it.
    const MemoryStats after = allocator().memory_stats();
    ASSERT_LT(memory->heap_index, after.heap_count);
    EXPECT_EQ(
        after.heaps[memory->heap_index].reserved_bytes,
        before.heaps[memory->heap_index].reserved_bytes + exported.memory_size);
  }

  // Without a view, and shared by two families where the device has them.
  ImageDesc bare = plane_desc(VK_FORMAT_R8_UNORM);
  bare.with_view = false;
  const bool two = physical().queue_families().size() > 1;
  const std::uint32_t own = device().queue_family();
  const std::uint32_t both[2] = {own, own == 0 ? 1U : 0U};
  if (two) {
    bare.queue_families = both;
    bare.queue_family_count = 2;
  }
  Result<ExportedImage> made =
      create_exported_image(device(), allocator(), bare);
  ASSERT_TRUE(made.ok()) << made.status().message();
  EXPECT_EQ(made->image.view(), VK_NULL_HANDLE);
  EXPECT_EQ(made->image.sharing_mode(),
            two ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE);
}

TEST_F(ImageExportTest, RefusesWhatItCannotExport) {
  const auto domain = [&](const ImageDesc& desc) {
    return create_exported_image(device(), allocator(), desc).status().domain();
  };
  const auto invalid = [&](const auto& change) {
    ImageDesc desc = plane_desc(VK_FORMAT_R8_UNORM);
    change(desc);
    return domain(desc) == Status::Code::InvalidArgument;
  };
  // What create_image refuses.
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.extent = {0, 32}; }));
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.usage = 0; }));
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.format = VK_FORMAT_UNDEFINED; }));
  EXPECT_TRUE(
      invalid([](ImageDesc& d) { d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT; }));
  const auto missing =
      static_cast<std::uint32_t>(physical().queue_families().size());
  EXPECT_TRUE(invalid([&](ImageDesc& d) {
    d.queue_families = &missing;
    d.queue_family_count = 1;
  }));
  // Anything but a single-plane 2D image of one mip, layer and sample, with
  // optimal tiling.
  EXPECT_TRUE(invalid([](ImageDesc& d) {
    d.type = VK_IMAGE_TYPE_3D;
    d.depth = 2;
  }));
  EXPECT_TRUE(invalid([](ImageDesc& d) {
    d.type = VK_IMAGE_TYPE_1D;
    d.extent.height = 1;
  }));
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.mip_levels = 2; }));
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.array_layers = 2; }));
  EXPECT_TRUE(invalid([](ImageDesc& d) {
    d.cube = true;
    d.array_layers = 6;
    d.extent = {32, 32};
  }));
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.samples = VK_SAMPLE_COUNT_4_BIT; }));
  EXPECT_TRUE(invalid([](ImageDesc& d) { d.tiling = VK_IMAGE_TILING_LINEAR; }));
  // Each refused as what an exported image is not, before create_image's
  // checks, whose advice -- six layers for a cube, one mip for a
  // multisampled image, no default view for a multi-planar format -- would
  // lead only to this refusal.
  const auto says = [&](const auto& change, const char* what) {
    ImageDesc desc = plane_desc(VK_FORMAT_R8_UNORM);
    change(desc);
    const Status refused =
        create_exported_image(device(), allocator(), desc).status();
    return refused.domain() == Status::Code::InvalidArgument &&
           refused.message().find(what) != std::string::npos;
  };
  constexpr const char* kShape = "an exported image is 2D";
  EXPECT_TRUE(says([](ImageDesc& d) { d.cube = true; }, kShape));
  EXPECT_TRUE(says(
      [](ImageDesc& d) {
        d.samples = VK_SAMPLE_COUNT_4_BIT;
        d.mip_levels = 2;
      },
      kShape));
  for (const bool with_view : {true, false}) {
    EXPECT_TRUE(says(
        [&](ImageDesc& d) {
          d.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
          d.with_view = with_view;
        },
        "export each plane"));
  }
  // Wider than the device exports.
  const VkExtent3D largest =
      opaque_fd_image(physical().handle(), VK_FORMAT_R8_UNORM).largest;
  if (largest.width < UINT32_MAX) {
    ImageDesc wide = plane_desc(VK_FORMAT_R8_UNORM);
    wide.extent = {largest.width + 1, 1};
    EXPECT_EQ(domain(wide), Status::Code::Unsupported);
  }
}

// The descriptor names the image's memory: imported into a second device on
// the same GPU, as CUDA imports it, an image created alike there reads what
// was written through the export, and the export reads what was written
// through the import. Each crossing is one side's release to outside Vulkan
// and the other's acquire from there, the contents moving between layouts as
// they go, under the validation layer.
TEST_F(ImageExportTest, TheDescriptorImportsTheSameImage) {
  constexpr VkFormat kFormat = VK_FORMAT_R8G8_UNORM;  // two bytes a texel
  if ((opaque_fd_image(physical().handle(), kFormat).features &
       VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
    GTEST_SKIP() << "the device exports opaque descriptors but cannot import";
  }
  const std::size_t bytes =
      std::size_t{kPlane.width} * kPlane.height * texel_bytes(kFormat);
  const std::vector<std::uint8_t> first = texels(bytes, 7);
  const std::vector<std::uint8_t> second = texels(bytes, 13);
  std::vector<std::uint8_t> got(bytes, 0);

  Result<ExportedImage> made =
      create_exported_image(device(), allocator(), plane_desc(kFormat));
  ASSERT_TRUE(made.ok()) << made.status().message();
  // Its descriptor is closed with it on any early return, until the import
  // takes it.
  ExportedImage exported = *std::move(made);
  const std::optional<MemoryInfo> memory = exported.image.memory_info();
  if (!memory.has_value()) {
    FAIL() << "no memory info";
  }
  Result<Buffer> landing_made = device_storage_buffer(allocator(), bytes);
  ASSERT_TRUE(landing_made.ok());
  const Buffer landing = *std::move(landing_made);

  // Written, and handed over in GENERAL, as CUDA takes it.
  ASSERT_TRUE(test::write_image(device(), allocator(), exported.image,
                                first.data(), bytes,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                  .ok());
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch
                    .release(exported.image, VK_QUEUE_FAMILY_EXTERNAL,
                             VK_IMAGE_LAYOUT_GENERAL)
                    .ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  exported.image.set_layout(VK_IMAGE_LAYOUT_GENERAL);

  // The importer: a device of its own, with an allocator for its batches.
  Result<Device> other_made =
      Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other_made.ok()) << other_made.status().message();
  const Device other = *std::move(other_made);
  Result<Allocator> other_allocator_made =
      Allocator::create(instance().handle(), other);
  ASSERT_TRUE(other_allocator_made.ok());
  Allocator other_allocator = *std::move(other_allocator_made);
  Result<Buffer> other_landing_made =
      device_storage_buffer(other_allocator, bytes);
  ASSERT_TRUE(other_landing_made.ok());
  const Buffer other_landing = *std::move(other_landing_made);

  // Created as the export's image was, on memory dedicated to it as the
  // export's was, each owned from the start, memory first.
  VkDevice vk = other.handle();
  UniqueHandle<VkDeviceMemory, vkFreeMemory> backing;
  VkExternalMemoryImageCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  // Zeroed, then set: samples has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.pNext = &external;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = kFormat;
  info.extent = {kPlane.width, kPlane.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = kImageUsage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage raw_image = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImage(vk, &info, nullptr, &raw_image), VK_SUCCESS);
  const UniqueHandle<VkImage, vkDestroyImage> imported(vk, raw_image);
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkImportMemoryFdInfoKHR import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  import.fd = exported.fd.get();
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = &import;
  dedicated.image = imported.get();
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &dedicated;
  alloc.allocationSize = exported.memory_size;
  alloc.memoryTypeIndex = memory->type_index;
  VkDeviceMemory raw_memory = VK_NULL_HANDLE;
  // A failed import leaves the descriptor with `exported`, which closes it.
  ASSERT_EQ(vkAllocateMemory(vk, &alloc, nullptr, &raw_memory), VK_SUCCESS)
      << "importing the descriptor failed";
  exported.fd.release();  // the import owns it now
  backing = UniqueHandle<VkDeviceMemory, vkFreeMemory>(vk, raw_memory);
  ASSERT_EQ(vkBindImageMemory(vk, imported.get(), backing.get(), 0),
            VK_SUCCESS);
  // A view for the batches, in the layout the export handed it over in; the
  // owners above free the image and memory.
  ImageInfo view_info;
  view_info.image = imported.get();
  view_info.format = kFormat;
  view_info.extent = info.extent;
  view_info.usage = kImageUsage;
  view_info.layout = VK_IMAGE_LAYOUT_GENERAL;
  view_info.memory = memory;
  Image view(view_info, nullptr);

  // Taken over into the layout the copy after it reads.
  {
    CommandBatch batch(other, other_allocator);
    ASSERT_TRUE(batch
                    .acquire(view, VK_QUEUE_FAMILY_EXTERNAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                    .ok());
    ASSERT_TRUE(
        batch.copy(view, kPlane.width, kPlane.height, other_landing, 0).ok());
    ASSERT_TRUE(batch.readback(other_landing, 0, bytes, got.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  view.set_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(got, first);

  // Written through the import, and handed back.
  ASSERT_TRUE(test::write_image(other, other_allocator, view, second.data(),
                                bytes, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                  .ok());
  {
    CommandBatch batch(other, other_allocator);
    ASSERT_TRUE(
        batch.release(view, VK_QUEUE_FAMILY_EXTERNAL, VK_IMAGE_LAYOUT_GENERAL)
            .ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  view.set_layout(VK_IMAGE_LAYOUT_GENERAL);

  // The exporter takes it back, reads it, and hands it over again: the
  // release moves the contents from the acquire's layout.
  std::fill(got.begin(), got.end(), std::uint8_t{0});
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch
                    .acquire(exported.image, VK_QUEUE_FAMILY_EXTERNAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                    .ok());
    ASSERT_TRUE(
        batch.copy(exported.image, kPlane.width, kPlane.height, landing, 0)
            .ok());
    ASSERT_TRUE(batch.readback(landing, 0, bytes, got.data()).ok());
    ASSERT_TRUE(batch
                    .release(exported.image, VK_QUEUE_FAMILY_EXTERNAL,
                             VK_IMAGE_LAYOUT_GENERAL)
                    .ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  exported.image.set_layout(VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(got, second);
}

// An exported image moves as its parts do, keeps itself on a self-move, and
// frees its memory with itself: its heap's share returns to what it was.
TEST_F(ImageExportTest, MovesAndFreesItsMemory) {
  const MemoryStats before = allocator().memory_stats();
  std::uint32_t heap = 0;
  {
    Result<ExportedImage> made = create_exported_image(
        device(), allocator(), plane_desc(VK_FORMAT_R8_UNORM));
    ASSERT_TRUE(made.ok()) << made.status().message();
    ExportedImage first = *std::move(made);
    const std::optional<MemoryInfo> memory = first.image.memory_info();
    if (!memory.has_value()) {
      FAIL() << "no memory info";
    }
    heap = memory->heap_index;
    VkImage handle = first.image.handle();
    const int fd = first.fd.get();
    const VkDeviceSize size = first.memory_size;

    ExportedImage second = std::move(first);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(first.image.valid());
    EXPECT_FALSE(first.fd.valid());
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(second.image.handle(), handle);
    EXPECT_EQ(second.fd.get(), fd);
    EXPECT_EQ(second.memory_size, size);
    ExportedImage* self = &second;
    second = std::move(*self);
    EXPECT_EQ(second.image.handle(), handle);
    EXPECT_EQ(second.fd.get(), fd);
    ExportedImage third;
    third = std::move(second);
    EXPECT_EQ(third.image.handle(), handle);
    EXPECT_EQ(third.fd.get(), fd);
    EXPECT_EQ(allocator().memory_stats().heaps[heap].reserved_bytes,
              before.heaps[heap].reserved_bytes + size);
  }
  EXPECT_EQ(allocator().memory_stats().heaps[heap].reserved_bytes,
            before.heaps[heap].reserved_bytes);
}

// The fixture's default device does not enable host import, so it refuses,
// naming the extension to enable.
TEST_F(MemoryTypeTest, ADeviceWithoutHostImportRefusesIt) {
  ASSERT_FALSE(
      device().extension_enabled(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME));
  const Status refused =
      create_imported_host_buffer(device(), allocator(), 4096).status();
  EXPECT_EQ(refused.domain(), Status::Code::Unsupported);
  EXPECT_NE(refused.message().find(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME),
            std::string::npos)
      << refused.message();
}

// The alignment `physical` imports host pointers at.
VkDeviceSize import_alignment(VkPhysicalDevice physical) {
  VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{};
  host.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &host;
  vkGetPhysicalDeviceProperties2(physical, &properties);
  return host.minImportedHostPointerAlignment;
}

// What `physical` can do with host memory as a copy source: import it.
VkExternalMemoryFeatureFlags host_import_features(VkPhysicalDevice physical) {
  // Zeroed, then set: handleType has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalBufferInfo query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
  query.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkExternalBufferProperties can{};
  can.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
  vkGetPhysicalDeviceExternalBufferProperties(physical, &query, &can);
  return can.externalMemoryProperties.externalMemoryFeatures;
}

// The memory types `device` lets the host pages at `pages` be imported into
// (vkGetMemoryHostPointerPropertiesEXT), or 0.
std::uint32_t host_pointer_types(const Device& device, const void* pages) {
  const auto query = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
      vkGetDeviceProcAddr(device.handle(),
                          "vkGetMemoryHostPointerPropertiesEXT"));
  if (query == nullptr) return 0;
  VkMemoryHostPointerPropertiesEXT properties{};
  properties.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
  const VkResult got = query(
      device.handle(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      pages, &properties);
  return got == VK_SUCCESS ? properties.memoryTypeBits : 0;
}

// The first byte where `got` and `want` differ, or their size where none
// does: a mismatch reports one index, not two pictures' bytes.
std::size_t first_difference(const std::vector<std::uint8_t>& got,
                             const std::vector<std::uint8_t>& want) {
  const auto at =
      std::mismatch(got.begin(), got.end(), want.begin(), want.end()).first;
  return static_cast<std::size_t>(at - got.begin());
}

// A device asked for host import where the driver offers it.
class HostImportTest : public test::VulkanDeviceTest {
 protected:
  DeviceRequirements requirements() const override {
    DeviceRequirements r;
    r.optional_extensions = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    return r;
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    // Refused, not failed, wherever the import is impossible.
    if (!device().extension_enabled(
            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ||
        (host_import_features(physical().handle()) &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
      EXPECT_EQ(create_imported_host_buffer(device(), allocator(), 4096)
                    .status()
                    .domain(),
                Status::Code::Unsupported);
      GTEST_SKIP() << "the device cannot import host memory as a copy source";
    }
  }
};

// The pages a writer fills are what a copy on the device reads, at one
// 2160p v210 picture (22,118,400 bytes, a multiple of every import
// alignment up to 16 KiB) and at a size the alignment rounds up (10,000
// bytes: 12,288 at 4 KiB). A second picture written into the same pages is
// what the next copy reads: the buffer is the pages, not a copy of them.
TEST_F(HostImportTest, ACopyReadsWhatIsWrittenThroughThePages) {
  const VkDeviceSize alignment = import_alignment(physical().handle());
  ASSERT_GT(alignment, 0U);
  const VkPhysicalDeviceMemoryProperties& props =
      physical().memory_properties();
  for (const VkDeviceSize asked :
       {VkDeviceSize{22118400}, VkDeviceSize{10000}}) {
    SCOPED_TRACE(asked);
    const VkDeviceSize rounded =
        (asked + alignment - 1) / alignment * alignment;
    const MemoryStats before = allocator().memory_stats();
    std::uint32_t heap = 0;
    {
      Result<ImportedHostBuffer> made =
          create_imported_host_buffer(device(), allocator(), asked);
      ASSERT_TRUE(made.ok()) << made.status().message();
      const ImportedHostBuffer pages = *std::move(made);
      EXPECT_EQ(pages.bytes, rounded);
      ASSERT_NE(pages.data, nullptr);
      EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pages.data) % alignment, 0U);
      // A copy source only, unmapped: the host has the pages.
      EXPECT_EQ(pages.buffer.size(), rounded);
      EXPECT_EQ(pages.buffer.usage(),
                VkBufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_SRC_BIT});
      EXPECT_EQ(pages.buffer.mapped(), nullptr);
      const std::optional<MemoryInfo> memory = pages.buffer.memory_info();
      if (!memory.has_value()) {
        FAIL() << "no memory info";
      }
      ASSERT_LT(memory->type_index, props.memoryTypeCount);
      constexpr VkMemoryPropertyFlags kCoherent =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      EXPECT_EQ(memory->properties & kCoherent, kCoherent);
      if (!physical().unified_memory()) {
        EXPECT_FALSE(pages.buffer.is_device_local());
      }
      // In a type the pages allow, and cached where they allow a coherent
      // cached host type: the pages are ordinary host memory.
      const std::uint32_t page_types = host_pointer_types(device(), pages.data);
      EXPECT_NE(page_types & (1U << memory->type_index), 0U)
          << "type " << memory->type_index << " of 0x" << std::hex
          << page_types;
      constexpr VkMemoryPropertyFlags kCached =
          kCoherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      bool cached_allowed = false;
      for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
        if ((page_types & (1U << i)) != 0 && (flags & kCached) == kCached &&
            (physical().unified_memory() ||
             (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)) {
          cached_allowed = true;
        }
      }
      if (cached_allowed) {
        EXPECT_NE(memory->properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0U)
            << "type " << memory->type_index << " of 0x" << std::hex
            << page_types;
      }
      // Through the allocator: its heap's share counts the pages.
      heap = memory->heap_index;
      ASSERT_LT(heap, before.heap_count);
      EXPECT_EQ(allocator().memory_stats().heaps[heap].reserved_bytes,
                before.heaps[heap].reserved_bytes + rounded);

      Result<Buffer> landing_made = device_storage_buffer(allocator(), rounded);
      ASSERT_TRUE(landing_made.ok()) << landing_made.status().message();
      const Buffer landing = *std::move(landing_made);
      for (const std::uint32_t seed : {7U, 13U}) {
        SCOPED_TRACE(seed);
        const std::vector<std::uint8_t> picture =
            texels(static_cast<std::size_t>(rounded), seed);
        std::copy(picture.begin(), picture.end(), pages.data);
        std::vector<std::uint8_t> got(picture.size(), 0);
        CommandBatch batch(device(), allocator());
        ASSERT_TRUE(batch.copy(pages.buffer, 0, landing, 0, rounded).ok());
        ASSERT_TRUE(batch.readback(landing, 0, rounded, got.data()).ok());
        ASSERT_TRUE(batch.submit().ok());
        EXPECT_EQ(first_difference(got, picture), picture.size());
      }
    }
    // The buffer freed its memory, and with it the pages. Allocations, not
    // reservations: on unified memory the landing buffer's block may stay.
    EXPECT_EQ(allocator().memory_stats().heaps[heap].allocation_bytes,
              before.heaps[heap].allocation_bytes);
  }
}

// 0 bytes is refused, and so is an allocator that is not the device's own,
// before anything is made: its memory could not be bound to this device's
// buffer.
TEST_F(HostImportTest, RefusesNoBytesAndAnotherDevicesAllocator) {
  EXPECT_EQ(
      create_imported_host_buffer(device(), allocator(), 0).status().domain(),
      Status::Code::InvalidArgument);
  Result<Device> other = Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other.ok()) << other.status().message();
  Result<Allocator> foreign = Allocator::create(instance().handle(), *other);
  ASSERT_TRUE(foreign.ok()) << foreign.status().message();
  EXPECT_EQ(
      create_imported_host_buffer(device(), *foreign, 4096).status().domain(),
      Status::Code::InvalidArgument);
  const Allocator taken = *std::move(foreign);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(
      create_imported_host_buffer(device(), *foreign, 4096).status().domain(),
      Status::Code::InvalidArgument);
}

// --- a timeline another API imports ------------------------------------------

// What `physical` can do with a timeline semaphore as an opaque descriptor:
// export it, import it; nothing where the handle type is not compatible.
VkExternalSemaphoreFeatureFlags opaque_fd_timeline(VkPhysicalDevice physical) {
  VkSemaphoreTypeCreateInfo timeline{};
  timeline.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  // Zeroed, then set: handleType has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalSemaphoreInfo query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
  query.pNext = &timeline;
  query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkExternalSemaphoreProperties can{};
  can.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
  vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &query, &can);
  if ((can.compatibleHandleTypes &
       VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) == 0) {
    return 0;
  }
  return can.externalSemaphoreFeatures;
}

// Longer than any of these waits should take, so a broken one fails the test
// rather than hanging it.
constexpr std::uint64_t kLongWaitNs = 10'000'000'000;

using Timeline = UniqueHandle<VkSemaphore, vkDestroySemaphore>;

// The counter of a semaphore on `device`, or UINT64_MAX where it cannot be
// read: the importer's side, which has no TimelineSemaphore.
std::uint64_t counter(VkDevice device, VkSemaphore semaphore) {
  std::uint64_t value = 0;
  return vkGetSemaphoreCounterValue(device, semaphore, &value) == VK_SUCCESS
             ? value
             : UINT64_MAX;
}

// A host signal of `value` on a semaphore on `device`.
VkResult host_signal(VkDevice device, VkSemaphore semaphore,
                     std::uint64_t value) {
  VkSemaphoreSignalInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
  info.semaphore = semaphore;
  info.value = value;
  return vkSignalSemaphore(device, &info);
}

// A device asked for semaphore export where the driver offers it. Its
// refusals run on every device; the tests that export skip where it cannot.
class TimelineExportTest : public test::VulkanDeviceTest {
 protected:
  DeviceRequirements requirements() const override {
    DeviceRequirements r;
    r.optional_extensions = {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    return r;
  }

  // Whether the device cannot export a timeline semaphore as an opaque
  // descriptor: without the extension, or with it where the driver exports
  // no such semaphore. Then the call must refuse it, not fail.
  bool cannot_export() {
    if (device().exports_semaphores() &&
        (opaque_fd_timeline(physical().handle()) &
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0) {
      return false;
    }
    EXPECT_EQ(create_exported_timeline(device()).status().domain(),
              Status::Code::Unsupported);
    return true;
  }

  // For a host signal of the exported timeline after the host read it. The
  // validation layer 1.3.275 (Ubuntu 24.04's) records a value read from an
  // exported timeline as a signal still pending, and reports the next host
  // signal as not below it (VUID-VkSemaphoreSignalInfo-value-03259), though
  // nothing is pending; 1.4.341 reports nothing. On an older layer the test's
  // errors from here on are counted, so everything before stays validated
  // there, and all of it on a newer layer.
  void allow_the_old_layers_pending_read() {
    constexpr std::uint32_t kClean = VK_MAKE_API_VERSION(0, 1, 4, 341);
    const std::uint32_t layer = test::validation_layer_version();
    if (layer != 0 && layer < kClean) allow_validation_errors();
  }

  // Whether the device can import its own timeline's descriptor as well.
  bool cannot_import() const {
    return (opaque_fd_timeline(physical().handle()) &
            VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0;
  }

  // The importer: `exported`'s descriptor imported for good into a timeline
  // semaphore made on `other`, as CUDA imports one. A successful import owns
  // the descriptor; a failed one leaves it with `exported`, which closes it.
  static Result<Timeline> import_timeline(const Device& other,
                                          ExportedTimeline& exported) {
    const auto import = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(other.handle(), "vkImportSemaphoreFdKHR"));
    if (import == nullptr) {
      return Status::unsupported("no vkImportSemaphoreFdKHR");
    }
    // Created as the export's was, a timeline
    // (VUID-VkImportSemaphoreFdInfoKHR-handleType-03264). Its own payload,
    // value 0, goes with the import.
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    VkSemaphore raw = VK_NULL_HANDLE;
    if (vkCreateSemaphore(other.handle(), &info, nullptr, &raw) != VK_SUCCESS) {
      return Status::unsupported("vkCreateSemaphore on the importer failed");
    }
    Timeline imported(other.handle(), raw);
    // Zeroed, then set: handleType has no zero enumerator.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    VkImportSemaphoreFdInfoKHR from{};
    from.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    from.semaphore = imported.get();
    from.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    from.fd = exported.fd.get();
    if (import(other.handle(), &from) != VK_SUCCESS) {
      return Status::unsupported("importing the descriptor failed");
    }
    exported.fd.release();  // the import owns it now
    return imported;
  }
};

// The fixture's device enables the extension wherever it is offered; one that
// did not refuses, naming the extension. On every device.
TEST_F(TimelineExportTest, ADeviceWithoutSemaphoreExportRefusesIt) {
  Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  EXPECT_FALSE(made->exports_semaphores());
  const Status refused = create_exported_timeline(*made).status();
  EXPECT_EQ(refused.domain(), Status::Code::Unsupported);
  EXPECT_NE(refused.message().find(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME),
            std::string::npos)
      << refused.message();
}

// What TimelineSemaphore::create refuses, refused first, on every device: a
// moved-from device, and one without timelineSemaphore even where it asked
// for semaphore export.
TEST_F(TimelineExportTest, RefusesWhatTimelineSemaphoreCreateRefuses) {
  DeviceRequirements without = requirements();
  without.timeline_semaphore = false;
  Result<Device> made = Device::create(instance(), physical(), without);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Status refused = create_exported_timeline(*made).status();
  EXPECT_EQ(refused.domain(), Status::Code::Unsupported);
  EXPECT_NE(refused.message().find("timelineSemaphore"), std::string::npos)
      << refused.message();

  Device first = *std::move(made);
  const Device second = std::move(first);
  // Exporting from the moved-from device is the point: it must be refused.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(create_exported_timeline(first).status().domain(),
            Status::Code::InvalidArgument);
}

// A timeline at the value asked, with an open descriptor that closes with
// it. The host signals, reads and waits on it as on any timeline, and a
// wait that blocks counts on its device.
TEST_F(TimelineExportTest, ExportsATimelineAtItsInitialValue) {
  if (cannot_export()) {
    GTEST_SKIP() << "the device cannot export a timeline semaphore as an "
                    "opaque descriptor";
  }
  int fd = -1;
  {
    Result<ExportedTimeline> made = create_exported_timeline(device(), 5);
    ASSERT_TRUE(made.ok()) << made.status().message();
    // Never imported, so its descriptor is still its own, closed with it.
    ExportedTimeline written = *std::move(made);
    ASSERT_TRUE(written.semaphore.valid());
    EXPECT_EQ(written.semaphore.device(), device().handle());
    const Result<std::uint64_t> start = written.semaphore.value();
    ASSERT_TRUE(start.ok()) << start.status().message();
    EXPECT_EQ(*start, 5U);
    ASSERT_TRUE(written.fd.valid());
    fd = written.fd.get();
    EXPECT_NE(fcntl(fd, F_GETFD), -1);

    allow_the_old_layers_pending_read();
    ASSERT_TRUE(written.semaphore.signal(6).ok());
    EXPECT_TRUE(written.semaphore.wait(6).ok());
    const Result<std::uint64_t> raised = written.semaphore.value();
    ASSERT_TRUE(raised.ok()) << raised.status().message();
    EXPECT_EQ(*raised, 6U);
    // 7 is never set: a short wait for it blocks, times out, and counts.
    const DeviceCounters before = device().counters();
    EXPECT_TRUE(is_timeout(written.semaphore.wait(7, 1'000'000)));
    EXPECT_EQ(device().counters().host_waits - before.host_waits, 1U);
  }
  errno = 0;
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

// The descriptor names the same timeline: imported into a second device on
// the same GPU -- another context, as CUDA's is -- the importer reads the
// value set here, and its host's signal is read and waited for here.
TEST_F(TimelineExportTest, TheDescriptorImportsTheSameTimeline) {
  if (cannot_export()) {
    GTEST_SKIP() << "the device cannot export a timeline semaphore as an "
                    "opaque descriptor";
  }
  if (cannot_import()) {
    GTEST_SKIP() << "the device exports timeline descriptors but cannot "
                    "import them";
  }
  Result<ExportedTimeline> made = create_exported_timeline(device(), 3);
  ASSERT_TRUE(made.ok()) << made.status().message();
  ExportedTimeline written = *std::move(made);

  Result<Device> other_made =
      Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other_made.ok()) << other_made.status().message();
  const Device other = *std::move(other_made);
  // Declared after the device, so it goes first.
  Result<Timeline> imported_made = import_timeline(other, written);
  ASSERT_TRUE(imported_made.ok()) << imported_made.status().message();
  const Timeline imported = *std::move(imported_made);
  EXPECT_FALSE(written.fd.valid());
  EXPECT_EQ(counter(other.handle(), imported.get()), 3U);

  ASSERT_EQ(host_signal(other.handle(), imported.get(), 7), VK_SUCCESS);
  const Result<std::uint64_t> seen = written.semaphore.value();
  ASSERT_TRUE(seen.ok()) << seen.status().message();
  EXPECT_EQ(*seen, 7U);
  EXPECT_TRUE(written.semaphore.wait(7, kLongWaitNs).ok());

  allow_the_old_layers_pending_read();
  ASSERT_TRUE(written.semaphore.signal(8).ok());
  EXPECT_EQ(counter(other.handle(), imported.get()), 8U);
}

// Sets a timeline from the host on scope exit, unless it is there already,
// so a batch held for an importer's value never outlives a failed assertion:
// what ExportedTimeline asks of a caller whose importer cannot set it.
// Declared after the pending batch it releases, so it runs first.
class SetOnExit {
 public:
  SetOnExit(TimelineSemaphore& timeline, std::uint64_t value)
      : timeline_(timeline), value_(value) {}
  SetOnExit(const SetOnExit&) = delete;
  SetOnExit& operator=(const SetOnExit&) = delete;
  SetOnExit(SetOnExit&&) = delete;
  SetOnExit& operator=(SetOnExit&&) = delete;
  ~SetOnExit() {
    const Result<std::uint64_t> now = timeline_.value();
    if (now.ok() && *now < value_) {
      EXPECT_TRUE(timeline_.signal(value_).ok());
    }
  }

 private:
  TimelineSemaphore& timeline_;
  std::uint64_t value_;
};

// The property a decoder's hand-over relies on: a batch that waits for the
// importer's value is held on the device, with no host wait, until the
// importer sets it, then runs and sets its own. Submitted before the value
// is set -- against ExportedTimeline's order, with nothing else on the
// queue meanwhile -- so the hold is what is seen.
TEST_F(TimelineExportTest, ABatchWaitsOnTheDeviceForTheImporter) {
  if (cannot_export()) {
    GTEST_SKIP() << "the device cannot export a timeline semaphore as an "
                    "opaque descriptor";
  }
  if (cannot_import()) {
    GTEST_SKIP() << "the device exports timeline descriptors but cannot "
                    "import them";
  }
  Result<ExportedTimeline> made = create_exported_timeline(device());
  ASSERT_TRUE(made.ok()) << made.status().message();
  ExportedTimeline written = *std::move(made);
  Result<Device> other_made =
      Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other_made.ok()) << other_made.status().message();
  const Device other = *std::move(other_made);
  Result<Timeline> imported_made = import_timeline(other, written);
  ASSERT_TRUE(imported_made.ok()) << imported_made.status().message();
  const Timeline imported = *std::move(imported_made);
  Result<TimelineSemaphore> done_made = TimelineSemaphore::create(device());
  ASSERT_TRUE(done_made.ok()) << done_made.status().message();
  const TimelineSemaphore done = *std::move(done_made);

  constexpr std::uint32_t kWords = 64;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kWords} * 4;
  Result<Buffer> buffer_made = device_storage_buffer(allocator(), kBytes);
  ASSERT_TRUE(buffer_made.ok()) << buffer_made.status().message();
  const Buffer buffer = *std::move(buffer_made);
  std::vector<std::uint32_t> words(kWords);
  for (std::uint32_t i = 0; i < kWords; ++i) words[i] = (i * 2654435761U) + 9;
  std::vector<std::uint32_t> back(kWords, 0);

  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(buffer, 0, words.data(), kBytes).ok());
  ASSERT_TRUE(batch.readback(buffer, 0, kBytes, back.data()).ok());
  Result<PendingBatch> pending_made =
      batch.submit_async({{&written.semaphore, 9}}, {{&done, 1}});
  ASSERT_TRUE(pending_made.ok()) << pending_made.status().message();
  PendingBatch read = *std::move(pending_made);
  const SetOnExit release(written.semaphore, 9);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(read.ready());
  const Result<std::uint64_t> held = done.value();
  ASSERT_TRUE(held.ok()) << held.status().message();
  EXPECT_EQ(*held, 0U);

  // The importer sets 9 on its own device, and the batch runs.
  ASSERT_EQ(host_signal(other.handle(), imported.get(), 9), VK_SUCCESS);
  EXPECT_TRUE(done.wait(1, kLongWaitNs).ok());
  EXPECT_TRUE(read.wait(kLongWaitNs).ok());
  EXPECT_EQ(back, words);
}

}  // namespace
}  // namespace volumetric_kit::core
