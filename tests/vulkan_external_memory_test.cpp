// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// find_memory_type and create_exported_buffer, ported from recon's
// core_external_memory_test, plus a round trip recon could not make without
// CUDA: the exported descriptor imported back into Vulkan, reading the bytes
// written through the export. Export needs VK_KHR_external_memory_fd, which
// MoltenVK lacks; there the refusal is what is checked.

#include "volumetric_kit/core/vulkan/external_memory.hpp"

#include <unistd.h>

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
#include "vulkan_device_fixture.hpp"

namespace volumetric_kit::core {
namespace {

constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr VkMemoryPropertyFlags kMapped = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

using MemoryTypeTest = test::VulkanDeviceTest;

TEST_F(MemoryTypeTest, FindsThePlainestTypeThatFits) {
  const VkPhysicalDeviceMemoryProperties& memory =
      physical().memory_properties();
  const auto flags = [&](std::uint32_t i) {
    return memory.memoryTypes[i].propertyFlags;
  };
  const std::optional<std::uint32_t> local =
      find_memory_type(device(), ~0U, kLocal);
  // An if, not ASSERT_TRUE: clang-tidy's optional-access check follows it.
  if (!local.has_value()) {
    FAIL() << "no device-local memory type";
  }
  EXPECT_NE(flags(*local) & kLocal, 0U);
  for (std::uint32_t i = 0; i < *local; ++i) EXPECT_EQ(flags(i) & kLocal, 0U);
  // The spec's order is what keeps a device-local resource out of the BAR.
  bool unmapped_local = false;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    unmapped_local = unmapped_local ||
                     ((flags(i) & kLocal) != 0 && (flags(i) & kMapped) == 0);
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
        VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD}}) {
    EXPECT_FALSE(find_memory_type(device(), ~0U, unusable).has_value());
  }
}

// The fixture's default device asks for no export, so it has none.
TEST_F(MemoryTypeTest, ADeviceWithoutExportRefusesIt) {
  ASSERT_FALSE(device().exports_memory());
  EXPECT_EQ(create_exported_buffer(device(), 256).status().domain(),
            Status::Code::Unsupported);
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
    if (IsSkipped() || HasFatalFailure()) return;
    if (!device().exports_memory()) {
      EXPECT_EQ(create_exported_buffer(device(), 256).status().domain(),
                Status::Code::Unsupported);
      GTEST_SKIP() << "the device offers no VK_KHR_external_memory_fd";
    }
  }
};

TEST_F(ExportTest, ExportsADeviceOnlyStorageBuffer) {
  EXPECT_EQ(create_exported_buffer(device(), 0).status().domain(),
            Status::Code::InvalidArgument);
  constexpr VkDeviceSize kBytes = 4096;
  Result<ExportedBuffer> made = create_exported_buffer(device(), kBytes);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const ExportedBuffer exported = *std::move(made);
  EXPECT_GE(exported.fd, 0);
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
  // Placed as DeviceOnly: private wherever the buffer allows it.
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(device().handle(), exported.buffer.handle(),
                                &needs);
  if (find_memory_type(device(), needs.memoryTypeBits, kLocal, kMapped)) {
    EXPECT_EQ(memory->properties & kMapped, 0U);
  }
  EXPECT_EQ(close(exported.fd), 0);  // never imported: still ours
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
  constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalBufferInfo query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
  query.usage = kUsage;
  query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkExternalBufferProperties can{};
  can.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
  vkGetPhysicalDeviceExternalBufferProperties(physical().handle(), &query,
                                              &can);
  if ((can.externalMemoryProperties.externalMemoryFeatures &
       VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
    GTEST_SKIP() << "the device exports opaque descriptors but cannot import";
  }

  Result<ExportedBuffer> made = create_exported_buffer(device(), kBytes);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const ExportedBuffer exported = *std::move(made);
  const std::optional<MemoryInfo> memory = exported.buffer.memory_info();
  if (!memory.has_value()) {
    EXPECT_EQ(close(exported.fd), 0);
    FAIL() << "no memory info";
  }
  std::vector<std::uint32_t> words(kWords);
  for (std::uint32_t i = 0; i < kWords; ++i) words[i] = i * 2654435761U;

  // An ordinary storage buffer to a batch, taken over from outside Vulkan;
  // then released back outside, for the other device.
  std::vector<std::uint32_t> back(kWords, 0);
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.acquire(exported.buffer, VK_QUEUE_FAMILY_EXTERNAL).ok());
    ASSERT_TRUE(batch.upload(exported.buffer, 0, words.data(), kBytes).ok());
    ASSERT_TRUE(batch.readback(exported.buffer, 0, kBytes, back.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(back, words);
  const Status released = device().submit_single_time([&](VkCommandBuffer cmd) {
    VkBufferMemoryBarrier release{};
    release.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.srcQueueFamilyIndex = device().queue_family();
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.buffer = exported.buffer.handle();
    release.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                         &release, 0, nullptr);
  });
  ASSERT_TRUE(released.ok()) << released.message();

  // The importer: a device of its own, with an allocator for its batch.
  Result<Device> other_made =
      Device::create(instance(), physical(), requirements());
  ASSERT_TRUE(other_made.ok()) << other_made.status().message();
  const Device other = *std::move(other_made);
  Result<Allocator> other_allocator_made =
      Allocator::create(instance().handle(), other);
  ASSERT_TRUE(other_allocator_made.ok());
  Allocator other_allocator = *std::move(other_allocator_made);

  // Created as the export's buffer was, and dedicated as its memory was.
  VkDevice vk = other.handle();
  VkExternalMemoryBufferCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.pNext = &external;
  info.size = kBytes;
  info.usage = kUsage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer imported = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateBuffer(vk, &info, nullptr, &imported), VK_SUCCESS);
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkImportMemoryFdInfoKHR import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  import.fd = exported.fd;
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = &import;
  dedicated.buffer = imported;
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &dedicated;
  alloc.allocationSize = exported.memory_size;
  alloc.memoryTypeIndex = memory->type_index;
  VkDeviceMemory backing = VK_NULL_HANDLE;
  const VkResult allocated = vkAllocateMemory(vk, &alloc, nullptr, &backing);
  if (allocated != VK_SUCCESS) {
    vkDestroyBuffer(vk, imported, nullptr);
    EXPECT_EQ(close(exported.fd), 0);  // a failed import leaves it ours
    FAIL() << "importing the descriptor failed: " << allocated;
  }
  // The import owns the descriptor now.
  ASSERT_EQ(vkBindBufferMemory(vk, imported, backing, 0), VK_SUCCESS);
  const Buffer view(
      imported, kBytes, kUsage, VK_SHARING_MODE_EXCLUSIVE, nullptr,
      [vk, imported, backing] {
        vkDestroyBuffer(vk, imported, nullptr);
        vkFreeMemory(vk, backing, nullptr);
      },
      memory);
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
