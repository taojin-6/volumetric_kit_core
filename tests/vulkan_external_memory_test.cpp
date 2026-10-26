// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// find_memory_type and create_exported_buffer, ported from recon's
// core_external_memory_test, plus a round trip recon could not make without
// CUDA: the exported descriptor imported back into Vulkan, reading the bytes
// written through the export. create_exported_image likewise, its image
// crossing both ways through batches that release it to, and acquire it
// from, outside Vulkan. Export needs VK_KHR_external_memory_fd; where the
// device lacks it, the refusal is what is checked.

#include "volumetric_kit/core/vulkan/external_memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/write_image.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/format.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"

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

}  // namespace
}  // namespace volumetric_kit::core
