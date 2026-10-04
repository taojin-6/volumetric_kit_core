// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Descriptors, shader modules, fences and semaphores, command pools and
// buffers. Argument checks run without a device; the rest on one (or
// lavapipe), as vulkan_fixture.hpp describes.

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_buffer.hpp"
#include "volumetric_kit/core/vulkan/command_pool.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shader.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "vulkan_device_fixture.hpp"

namespace volumetric_kit::core {
namespace {

// `void main() {}` with local size 1x1x1: glslc --target-env=vulkan1.2
// (SPIR-V 1.5).
constexpr std::uint32_t kEmptyCompute[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x0000000a, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000009, 0x0000000b,
    0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007,
    0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001,
    0x0006002c, 0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x000100fd, 0x00010038};

bool is_invalid(const Status& s) {
  return s.domain() == Status::Code::InvalidArgument;
}

// A distinct non-null handle of any kind, for a check that must fire before
// Vulkan sees it: a handle is a pointer on 64-bit targets and a uint64_t on
// 32-bit ones.
char handle_storage[2];

template <class Handle>
Handle fake_handle(int index) {
  char* address = &handle_storage[index];
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(address);
  } else {
    return static_cast<Handle>(reinterpret_cast<std::uintptr_t>(address));
  }
}

// The counter, or a failure and 0.
std::uint64_t counter(const TimelineSemaphore& timeline) {
  const Result<std::uint64_t> value = timeline.value();
  EXPECT_TRUE(value.ok()) << value.status().message();
  return value.ok() ? *value : 0;
}

// --- without a device --------------------------------------------------------

TEST(VulkanObjects, RefuseANullDevice) {
  const VkDescriptorSetLayoutBinding binding{
      0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
      nullptr};
  EXPECT_TRUE(is_invalid(
      DescriptorSetLayout::create(VK_NULL_HANDLE, &binding, 1).status()));
  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  EXPECT_TRUE(
      is_invalid(DescriptorPool::create(VK_NULL_HANDLE, &size, 1, 1).status()));
  EXPECT_TRUE(is_invalid(
      ShaderModule::create(VK_NULL_HANDLE, kEmptyCompute, sizeof(kEmptyCompute))
          .status()));
  EXPECT_TRUE(is_invalid(Fence::create(VK_NULL_HANDLE).status()));
  EXPECT_TRUE(is_invalid(Semaphore::create(VK_NULL_HANDLE).status()));
  EXPECT_TRUE(is_invalid(TimelineSemaphore::create(VK_NULL_HANDLE).status()));
  EXPECT_TRUE(is_invalid(CommandPool::create(VK_NULL_HANDLE, 0).status()));
}

TEST(VulkanObjects, RefuseMalformedArguments) {
  EXPECT_TRUE(is_invalid(
      DescriptorSetLayout::create(VK_NULL_HANDLE, nullptr, 1).status()));
  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  EXPECT_TRUE(is_invalid(
      DescriptorPool::create(VK_NULL_HANDLE, nullptr, 1, 1).status()));
  EXPECT_TRUE(
      is_invalid(DescriptorPool::create(VK_NULL_HANDLE, &size, 0, 1).status()));
  EXPECT_TRUE(
      is_invalid(DescriptorPool::create(VK_NULL_HANDLE, &size, 1, 0).status()));
  EXPECT_TRUE(
      is_invalid(ShaderModule::create(VK_NULL_HANDLE, nullptr, 4).status()));
  EXPECT_TRUE(is_invalid(
      ShaderModule::create(VK_NULL_HANDLE, kEmptyCompute, 0).status()));
  EXPECT_TRUE(is_invalid(
      ShaderModule::create(VK_NULL_HANDLE, kEmptyCompute, 7).status()));
}

TEST(VulkanObjects, EmptyObjectsReportRatherThanCrash) {
  Fence fence;
  EXPECT_FALSE(fence.valid());
  EXPECT_TRUE(is_invalid(fence.wait(0)));
  EXPECT_TRUE(is_invalid(fence.reset()));
  EXPECT_FALSE(fence.is_signaled());

  TimelineSemaphore timeline;
  EXPECT_TRUE(is_invalid(timeline.value().status()));
  EXPECT_TRUE(is_invalid(timeline.signal(1)));
  EXPECT_TRUE(is_invalid(timeline.wait(1, 0)));

  CommandPool pool;
  EXPECT_TRUE(is_invalid(pool.allocate_primary().status()));
  CommandBuffer cmd;
  EXPECT_TRUE(is_invalid(cmd.begin()));
  EXPECT_TRUE(is_invalid(cmd.end()));

  DescriptorPool descriptors;
  EXPECT_TRUE(is_invalid(descriptors.allocate(VK_NULL_HANDLE).status()));
  const DescriptorSet set;
  EXPECT_FALSE(set.valid());
  EXPECT_EQ(set.writes(), 0u);
  EXPECT_FALSE(ShaderModule{}.valid());
  EXPECT_FALSE(Semaphore{}.valid());
  EXPECT_FALSE(DescriptorSetLayout{}.valid());
}

// A null resource is valid only under nullDescriptor, which the tier never
// enables; each write stops at it before Vulkan is called.
TEST(VulkanObjectsDeathTest, WritesRefuseANullResource) {
  const DescriptorSet set(fake_handle<VkDevice>(0),
                          fake_handle<VkDescriptorSet>(1));
  ASSERT_TRUE(set.valid());
  EXPECT_DEATH(set.write_storage_buffer(0, VK_NULL_HANDLE, 0, VK_WHOLE_SIZE),
               "write_storage_buffer: the buffer is null");
  EXPECT_DEATH(set.write_uniform_buffer(0, VK_NULL_HANDLE, 0, VK_WHOLE_SIZE),
               "write_uniform_buffer: the buffer is null");
  EXPECT_DEATH(set.write_combined_image_sampler(
                   0, VK_NULL_HANDLE, VK_NULL_HANDLE,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
               "write_combined_image_sampler: the image view is null");
  EXPECT_DEATH(
      set.write_storage_image(0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL),
      "write_storage_image: the image view is null");
  EXPECT_DEATH(DescriptorSet{}.write_storage_image(0, VK_NULL_HANDLE,
                                                   VK_IMAGE_LAYOUT_GENERAL),
               "write_storage_image on an empty set");
  EXPECT_EQ(set.writes(), 0u);
}

// --- on a device -------------------------------------------------------------

class ResourcesTest : public test::VulkanDeviceTest {
 protected:
  VkDevice vk() { return device().handle(); }

  // Submit @p cmd (or nothing) to the device's queue, signalling @p fence
  // and, when given, @p timeline to @p value.
  void submit(VkCommandBuffer cmd, VkFence fence,
              VkSemaphore timeline = VK_NULL_HANDLE, std::uint64_t value = 0) {
    VkTimelineSemaphoreSubmitInfo values{};
    values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    values.signalSemaphoreValueCount = 1;
    values.pSignalSemaphoreValues = &value;
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.commandBufferCount = cmd != VK_NULL_HANDLE ? 1 : 0;
    info.pCommandBuffers = &cmd;
    if (timeline != VK_NULL_HANDLE) {
      info.pNext = &values;
      info.signalSemaphoreCount = 1;
      info.pSignalSemaphores = &timeline;
    }
    ASSERT_EQ(device().queue_submit(1, &info, fence), VK_SUCCESS);
  }

  Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                MemoryUsage memory) {
    BufferDesc desc;
    desc.size = size;
    desc.usage = usage;
    desc.memory = memory;
    desc.mapped = memory != MemoryUsage::DeviceOnly;
    Result<Buffer> buffer = allocator().create_buffer(desc);
    EXPECT_TRUE(buffer.ok()) << buffer.status().message();
    return buffer.ok() ? *std::move(buffer) : Buffer{};
  }
};

// --- descriptors -------------------------------------------------------------

TEST_F(ResourcesTest, WritesEveryDescriptorKindIntoASet) {
  constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kCompute, nullptr},
      {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kCompute, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kCompute, nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kCompute, nullptr},
  };
  Result<DescriptorSetLayout> layout =
      DescriptorSetLayout::create(vk(), bindings, 4);
  ASSERT_TRUE(layout.ok()) << layout.status().message();
  EXPECT_TRUE(layout->valid());

  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
  };
  Result<DescriptorPool> pool = DescriptorPool::create(vk(), sizes, 4, 1);
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  EXPECT_TRUE(is_invalid(pool->allocate(VK_NULL_HANDLE).status()));
  Result<DescriptorSet> allocated = pool->allocate(layout->handle());
  ASSERT_TRUE(allocated.ok()) << allocated.status().message();
  const DescriptorSet set = *std::move(allocated);
  ASSERT_TRUE(set.valid());

  const Buffer storage =
      buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::DeviceOnly);
  const Buffer uniform = buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                MemoryUsage::DeviceMapped);
  ImageDesc sampled_desc;
  sampled_desc.extent = {4, 4};
  sampled_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  sampled_desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  Result<Image> sampled = allocator().create_image(sampled_desc);
  ASSERT_TRUE(sampled.ok()) << sampled.status().message();
  ImageDesc storage_desc;
  storage_desc.extent = {4, 4};
  storage_desc.format = VK_FORMAT_R32_SFLOAT;
  storage_desc.usage = VK_IMAGE_USAGE_STORAGE_BIT;
  Result<Image> storage_image = allocator().create_image(storage_desc);
  ASSERT_TRUE(storage_image.ok()) << storage_image.status().message();

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.maxLod = 1.0F;
  VkSampler sampler = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateSampler(vk(), &sampler_info, nullptr, &sampler),
            VK_SUCCESS);

  // A copy, deliberately: copies share the write count.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const DescriptorSet alias = set;
  set.write_storage_buffer(0, storage.handle(), 0, VK_WHOLE_SIZE);
  alias.write_uniform_buffer(1, uniform.handle(), 0, 256);
  set.write_combined_image_sampler(2, sampled->view(), sampler,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  alias.write_storage_image(3, storage_image->view(), VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(set.writes(), 4u);
  EXPECT_EQ(alias.writes(), 4u);
  EXPECT_EQ(alias.handle(), set.handle());

  vkDestroySampler(vk(), sampler, nullptr);
}

TEST_F(ResourcesTest, DescriptorObjectsMove) {
  Result<DescriptorSetLayout> made =
      DescriptorSetLayout::create(vk(), nullptr, 0);
  ASSERT_TRUE(made.ok()) << made.status().message();
  DescriptorSetLayout a = *std::move(made);
  const DescriptorSetLayout b = std::move(a);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_TRUE(b.valid());

  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  Result<DescriptorPool> pool = DescriptorPool::create(vk(), &size, 1, 1);
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  DescriptorPool moved = *std::move(pool);
  EXPECT_TRUE(moved.valid());
  EXPECT_TRUE(moved.allocate(b.handle()).ok());
}

// --- shader modules ----------------------------------------------------------

TEST_F(ResourcesTest, BuildsAShaderModuleFromSpirv) {
  Result<ShaderModule> shader =
      ShaderModule::create(vk(), kEmptyCompute, sizeof(kEmptyCompute));
  ASSERT_TRUE(shader.ok()) << shader.status().message();
  EXPECT_TRUE(shader->valid());
  const ShaderModule moved = *std::move(shader);
  EXPECT_NE(moved.handle(), VK_NULL_HANDLE);
}

// --- fences and semaphores ---------------------------------------------------

TEST_F(ResourcesTest, FenceSignalsWaitsTimesOutAndResets) {
  Result<Fence> unsignaled = Fence::create(vk());
  ASSERT_TRUE(unsignaled.ok()) << unsignaled.status().message();
  EXPECT_FALSE(unsignaled->is_signaled());
  const Status timed_out = unsignaled->wait(0);
  EXPECT_EQ(vk_result(timed_out), std::optional<VkResult>(VK_TIMEOUT));

  Result<Fence> signaled = Fence::create(vk(), /*signaled=*/true);
  ASSERT_TRUE(signaled.ok()) << signaled.status().message();
  EXPECT_TRUE(signaled->is_signaled());
  EXPECT_TRUE(signaled->wait(0).ok());
  ASSERT_TRUE(signaled->reset().ok());
  EXPECT_FALSE(signaled->is_signaled());

  submit(VK_NULL_HANDLE, unsignaled->handle());  // the queue signals it
  EXPECT_TRUE(unsignaled->wait().ok());
  EXPECT_TRUE(unsignaled->is_signaled());
}

TEST_F(ResourcesTest, MakesABinarySemaphore) {
  Result<Semaphore> semaphore = Semaphore::create(vk());
  ASSERT_TRUE(semaphore.ok()) << semaphore.status().message();
  const Semaphore moved = *std::move(semaphore);
  EXPECT_TRUE(moved.valid());
}

TEST_F(ResourcesTest, TimelineCountsUpFromTheHostAndTheQueue) {
  Result<TimelineSemaphore> made = TimelineSemaphore::create(vk(), 5);
  ASSERT_TRUE(made.ok()) << made.status().message();
  TimelineSemaphore timeline = *std::move(made);
  ASSERT_EQ(counter(timeline), 5u);

  ASSERT_TRUE(timeline.signal(7).ok());
  EXPECT_EQ(counter(timeline), 7u);
  EXPECT_TRUE(is_invalid(timeline.signal(7)));  // must advance
  EXPECT_TRUE(is_invalid(timeline.signal(6)));
  EXPECT_TRUE(timeline.wait(7, 0).ok());
  EXPECT_EQ(vk_result(timeline.wait(8, 0)),
            std::optional<VkResult>(VK_TIMEOUT));

  submit(VK_NULL_HANDLE, VK_NULL_HANDLE, timeline.handle(), 10);
  EXPECT_TRUE(timeline.wait(10).ok());
  EXPECT_EQ(counter(timeline), 10u);
}

// --- command pools and buffers -----------------------------------------------

TEST_F(ResourcesTest, RecordsSubmitsAndReRecordsACommandBuffer) {
  Result<CommandPool> made = CommandPool::create(vk(), device().queue_family());
  ASSERT_TRUE(made.ok()) << made.status().message();
  CommandPool pool = *std::move(made);
  EXPECT_EQ(pool.queue_family(), device().queue_family());
  Result<CommandBuffer> allocated = pool.allocate_primary();
  ASSERT_TRUE(allocated.ok()) << allocated.status().message();
  CommandBuffer cmd = *std::move(allocated);
  ASSERT_TRUE(cmd.valid());

  const Buffer target =
      buffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Staging);
  const auto* words = static_cast<const std::uint32_t*>(target.mapped());
  for (const std::uint32_t pattern : {0xC0FFEEu, 0xBADF00Du}) {
    // The pool's RESET_COMMAND_BUFFER flag lets begin() reset it in place.
    ASSERT_TRUE(cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT).ok());
    vkCmdFillBuffer(cmd.handle(), target.handle(), 0, VK_WHOLE_SIZE, pattern);
    test::host_read_barrier(cmd.handle());
    ASSERT_TRUE(cmd.end().ok());
    const Status s = device().submit_and_wait(cmd.handle());
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(words[0], pattern);
    EXPECT_EQ(words[15], pattern);
  }
}

TEST_F(ResourcesTest, CommandObjectsMove) {
  Result<CommandPool> made = CommandPool::create(vk(), device().queue_family());
  ASSERT_TRUE(made.ok()) << made.status().message();
  CommandPool a = *std::move(made);
  CommandPool b = std::move(a);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_TRUE(is_invalid(a.allocate_primary().status()));
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_TRUE(b.valid());

  Result<CommandBuffer> first = b.allocate_primary();
  Result<CommandBuffer> second = b.allocate_primary();
  ASSERT_TRUE(first.ok() && second.ok());
  CommandBuffer x = *std::move(first);
  VkCommandBuffer handle = x.handle();
  CommandBuffer y = std::move(x);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(x.valid());
  EXPECT_EQ(y.handle(), handle);
  y = *std::move(second);  // frees the first back to the pool
  EXPECT_NE(y.handle(), handle);
  EXPECT_TRUE(y.begin().ok());
  EXPECT_TRUE(y.end().ok());
}

}  // namespace
}  // namespace volumetric_kit::core
