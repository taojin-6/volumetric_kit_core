// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// find_memory_type, UniqueFd and create_exported_buffer, ported from recon's
// core_external_memory_test, plus a round trip recon could not make without
// CUDA: the exported descriptor imported back into Vulkan, reading the bytes
// written through the export. Export needs VK_KHR_external_memory_fd, which
// MoltenVK lacks; there the refusal is what is checked.

#include "volumetric_kit/core/vulkan/external_memory.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "vulkan_device_fixture.hpp"

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
  // The spec's order is what keeps a device-local resource out of the BAR,
  // wherever a general type the host cannot map exists: a mobile GPU's only
  // one may be lazily allocated, which is never chosen.
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
}

#ifndef _WIN32
// Whether `fd` names an open descriptor: only an open one duplicates.
bool is_open(int fd) {
  const int copy = dup(fd);
  if (copy < 0) return false;
  close(copy);
  return true;
}

TEST(UniqueFdTest, ClosesWhatItOwnsUnlessReleased) {
  int ends[2] = {-1, -1};
  ASSERT_EQ(pipe(ends), 0);
  {
    UniqueFd read_end(ends[0]);
    UniqueFd moved = std::move(read_end);
    // The moved-from state is what is checked.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(read_end.valid());
    EXPECT_EQ(moved.get(), ends[0]);
    // An assignment closes what the target held.
    UniqueFd write_end(ends[1]);
    moved = std::move(write_end);
    EXPECT_FALSE(is_open(ends[0]));
    EXPECT_EQ(moved.get(), ends[1]);
  }
  EXPECT_FALSE(is_open(ends[1]));  // closed with its owner

  ASSERT_EQ(pipe(ends), 0);
  UniqueFd kept(ends[0]);
  kept.reset();
  EXPECT_FALSE(is_open(ends[0]));
  EXPECT_FALSE(kept.valid());
  // Released, as to an import that took it: left open, and the caller's.
  UniqueFd handed(ends[1]);
  EXPECT_EQ(handed.release(), ends[1]);
  EXPECT_FALSE(handed.valid());
  EXPECT_TRUE(is_open(ends[1]));
  EXPECT_EQ(close(ends[1]), 0);
}
#endif

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
    if (IsSkipped() || HasFatalFailure()) return;
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
  // Allocated through the allocator, so its heap's usage counts it, and the
  // allocator's next allocation is budgeted with it.
  const MemoryStats after = allocator().memory_stats();
  ASSERT_LT(memory->heap_index, after.heap_count);
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

}  // namespace
}  // namespace volumetric_kit::core
