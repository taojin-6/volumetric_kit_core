// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// ComputePipeline, KernelSetBuilder, ComputeKernel, KernelSets, the one-shot
// dispatch, and the compute helpers. The shaders are tests/shaders/*.comp,
// compiled and embedded by vkc_embed_shaders. Argument checks run without a
// device; the rest on one (or lavapipe), as vulkan_fixture.hpp describes.

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "add_comp.spv.hpp"
#include "fill_comp.spv.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_kernel.hpp"
#include "volumetric_kit/core/vulkan/compute_pipeline.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/shader.hpp"
#include "vulkan_device_fixture.hpp"

namespace volumetric_kit::core {
namespace {

bool is_invalid(const Status& s) {
  return s.domain() == Status::Code::InvalidArgument;
}

// --- without a device --------------------------------------------------------

TEST(ComputeUtil, GroupCountRoundsUpWithoutWrapping) {
  EXPECT_EQ(group_count(0, 64), 0U);
  EXPECT_EQ(group_count(1, 64), 1U);
  EXPECT_EQ(group_count(64, 64), 1U);
  EXPECT_EQ(group_count(65, 64), 2U);
  // The (items + local - 1) / local idiom wraps here, to 0 groups.
  const std::uint32_t most = std::numeric_limits<std::uint32_t>::max();
  EXPECT_EQ(group_count(most, 64), (most / 64) + 1);
  EXPECT_EQ(group_count(most, 1), most);
}

TEST(ComputeUtil, ChecksAStorageRangeAgainstTheLimit) {
  EXPECT_TRUE(check_storage_buffer_range("t", 128, 128).ok());
  const Status over = check_storage_buffer_range("tsdf.voxels", 129, 128);
  EXPECT_TRUE(is_invalid(over));
  EXPECT_NE(over.message().find("tsdf.voxels"), std::string::npos);
  EXPECT_TRUE(is_invalid(check_storage_buffer_range(nullptr, 2, 1)));
}

// Through a pointer a Buffer would convert to host bytes, and the object
// itself be uploaded; host bytes of any type still convert.
static_assert(!std::is_constructible_v<StorageInput, const Buffer*>);
static_assert(!std::is_constructible_v<StorageInput, Buffer*>);
static_assert(std::is_constructible_v<StorageInput, const std::uint32_t*>);
static_assert(std::is_constructible_v<StorageInput, const Buffer&>);

TEST(ComputeUtil, StorageInputRefusesNullHostBytes) {
  const StorageInput none(static_cast<const void*>(nullptr));
  const Status s = none.check("integrate: depth", 4);
  EXPECT_TRUE(is_invalid(s));
  EXPECT_NE(s.message().find("integrate: depth"), std::string::npos);
  const std::uint32_t word = 0;
  EXPECT_TRUE(StorageInput(&word).check("t", 4).ok());
  const Buffer empty;
  EXPECT_TRUE(is_invalid(StorageInput(empty).check("t", 4)));
}

TEST(ComputePipelineArgs, RefusesANullShader) {
  ComputePipelineDesc desc;
  EXPECT_TRUE(
      is_invalid(ComputePipeline::create(VK_NULL_HANDLE, desc).status()));
  const ShaderModule empty;
  desc.shader = &empty;
  EXPECT_TRUE(
      is_invalid(ComputePipeline::create(VK_NULL_HANDLE, desc).status()));
}

TEST(ComputeKernelMoves, AnEmptyKernelMovesEmpty) {
  ComputeKernel a;
  EXPECT_FALSE(a.valid());
  const ComputeKernel b = std::move(a);
  EXPECT_FALSE(b.valid());
  EXPECT_EQ(b.name, nullptr);
}

// --- on a device -------------------------------------------------------------

class ComputeTest : public test::VulkanDeviceTest {
 protected:
  struct AddPush {
    std::uint32_t count;
    std::uint32_t delta;
  };

  static VkPushConstantRange range(std::uint32_t bytes) {
    VkPushConstantRange r{};
    r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    r.size = bytes;
    return r;
  }

  std::uint32_t max_groups() const {
    return physical().limits().maxComputeWorkGroupCount[0];
  }

  // The first `count` words of `buffer`, read back through a batch.
  std::vector<std::uint32_t> read(const Buffer& buffer, std::uint32_t count) {
    std::vector<std::uint32_t> out(count, 0xDEADBEEFU);
    CommandBatch batch(device(), allocator());
    EXPECT_TRUE(
        batch.readback(buffer, 0, VkDeviceSize{count} * 4, out.data()).ok());
    const Status s = batch.submit();
    EXPECT_TRUE(s.ok()) << s.message();
    return out;
  }
};

// The whole chain by hand -- shader, layout, pipeline, pool, set, record,
// dispatch, barrier, wait, read -- with no batch in the way.
TEST_F(ComputeTest, RunsAPipelineBuiltByHand) {
  constexpr std::uint32_t kCount = 1000;
  // Device-mapped, so the host reads the result in place: cheap on unified
  // memory, a PCIe read on a discrete GPU, which a library would avoid with a
  // CommandBatch readback -- here it keeps the chain free of batches.
  Result<Buffer> made = mapped_storage_buffer(
      allocator(), VkDeviceSize{kCount} * 4, HostAccess::Random);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer out = *std::move(made);

  Result<ShaderModule> shader = ShaderModule::create(
      device().handle(),
      // NOLINTNEXTLINE(*-reinterpret-cast)
      reinterpret_cast<const std::uint32_t*>(vkc_test_fill_comp_spv),
      vkc_test_fill_comp_spv_size);
  ASSERT_TRUE(shader.ok()) << shader.status().message();
  const VkDescriptorSetLayoutBinding binding{
      0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
      nullptr};
  Result<DescriptorSetLayout> layout =
      DescriptorSetLayout::create(device().handle(), &binding, 1);
  ASSERT_TRUE(layout.ok()) << layout.status().message();
  const VkDescriptorSetLayout layouts[] = {layout->handle()};
  const VkPushConstantRange push = range(4);
  ComputePipelineDesc desc;
  desc.shader = &*shader;
  desc.set_layouts = layouts;
  desc.set_layout_count = 1;
  desc.push_ranges = &push;
  desc.push_range_count = 1;
  Result<ComputePipeline> pipeline =
      ComputePipeline::create(device().handle(), desc);
  ASSERT_TRUE(pipeline.ok()) << pipeline.status().message();
  EXPECT_NE(pipeline->layout(), VK_NULL_HANDLE);

  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  Result<DescriptorPool> pool =
      DescriptorPool::create(device().handle(), &size, 1, 1);
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  Result<DescriptorSet> set = pool->allocate(layout->handle());
  ASSERT_TRUE(set.ok()) << set.status().message();
  set->write_storage_buffer(0, out.handle(), 0, VK_WHOLE_SIZE);

  const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->handle());
    VkDescriptorSet raw = set->handle();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->layout(), 0, 1, &raw, 0, nullptr);
    const std::uint32_t count = kCount;
    vkCmdPushConstants(cmd, pipeline->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       4, &count);
    vkCmdDispatch(cmd, group_count(kCount, 64), 1, 1);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                         0, nullptr);
  });
  ASSERT_TRUE(s.ok()) << s.message();
  const auto* values = static_cast<const std::uint32_t*>(out.mapped());
  for (std::uint32_t i = 0; i < kCount; ++i) ASSERT_EQ(values[i], i);
}

TEST_F(ComputeTest, PipelineRefusalsAndMoves) {
  Result<ShaderModule> shader = ShaderModule::create(
      device().handle(),
      // NOLINTNEXTLINE(*-reinterpret-cast)
      reinterpret_cast<const std::uint32_t*>(vkc_test_fill_comp_spv),
      vkc_test_fill_comp_spv_size);
  ASSERT_TRUE(shader.ok()) << shader.status().message();
  ComputePipelineDesc good;
  good.shader = &*shader;

  ComputePipelineDesc d = good;
  d.entry_point = nullptr;
  EXPECT_TRUE(
      is_invalid(ComputePipeline::create(device().handle(), d).status()));
  d = good;
  d.set_layout_count = 1;
  EXPECT_TRUE(
      is_invalid(ComputePipeline::create(device().handle(), d).status()));
  d = good;
  d.push_range_count = 1;
  EXPECT_TRUE(
      is_invalid(ComputePipeline::create(device().handle(), d).status()));
  EXPECT_TRUE(
      is_invalid(ComputePipeline::create(VK_NULL_HANDLE, good).status()));

  // A real layout, so the shader's binding 0 and push constant are declared.
  const VkDescriptorSetLayoutBinding binding{
      0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
      nullptr};
  Result<DescriptorSetLayout> layout =
      DescriptorSetLayout::create(device().handle(), &binding, 1);
  ASSERT_TRUE(layout.ok());
  const VkDescriptorSetLayout layouts[] = {layout->handle()};
  const VkPushConstantRange push = range(4);
  good.set_layouts = layouts;
  good.set_layout_count = 1;
  good.push_ranges = &push;
  good.push_range_count = 1;
  Result<ComputePipeline> made =
      ComputePipeline::create(device().handle(), good);
  ASSERT_TRUE(made.ok()) << made.status().message();
  ComputePipeline a = *std::move(made);
  VkPipeline handle = a.handle();
  ComputePipeline b = std::move(a);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_EQ(a.layout(), VK_NULL_HANDLE);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(b.handle(), handle);
  ComputePipeline* self = &b;
  b = std::move(*self);
  EXPECT_TRUE(b.valid());
}

TEST_F(ComputeTest, BuildsKernelsThatShareAPool) {
  const VkPushConstantRange add_push = range(sizeof(AddPush));
  const VkPushConstantRange fill_push = range(4);
  ComputeKernel add;
  ComputeKernel fill;
  KernelSetBuilder builder(device());
  ASSERT_TRUE(builder
                  .add(add, "test_add", vkc_test_add_comp_spv,
                       vkc_test_add_comp_spv_size, 1, &add_push)
                  .ok());
  ASSERT_TRUE(builder
                  .add(fill, "test_fill", vkc_test_fill_comp_spv,
                       vkc_test_fill_comp_spv_size, 1, &fill_push)
                  .ok());
  EXPECT_FALSE(add.valid());  // no set until build()
  const Result<DescriptorPool> pool = builder.build();
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  EXPECT_TRUE(add.valid() && fill.valid());
  EXPECT_STREQ(add.name, "test_add");
  EXPECT_EQ(add.push_bytes, sizeof(AddPush));
  EXPECT_EQ(add.bindings, 1U);
  EXPECT_NE(add.set.handle(), fill.set.handle());

  // Moves empty the source, set included.
  ComputeKernel moved = std::move(add);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(add.valid());
  EXPECT_EQ(add.set.handle(), VK_NULL_HANDLE);
  EXPECT_EQ(add.name, nullptr);
  EXPECT_EQ(add.push_bytes, 0U);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(moved.valid());
  fill = std::move(moved);
  EXPECT_TRUE(fill.valid());
  EXPECT_STREQ(fill.name, "test_add");
  ComputeKernel* self = &fill;
  fill = std::move(*self);
  EXPECT_TRUE(fill.valid());
}

TEST_F(ComputeTest, NamesTheKernelThatFailsToBuild) {
  ComputeKernel kernel;
  KernelSetBuilder builder(device());
  VkPushConstantRange off = range(8);
  off.offset = 4;
  const Status offset = builder.add(kernel, "misplaced", vkc_test_add_comp_spv,
                                    vkc_test_add_comp_spv_size, 1, &off);
  EXPECT_TRUE(is_invalid(offset));
  EXPECT_NE(offset.message().find("misplaced"), std::string::npos);
  EXPECT_EQ(kernel.name, nullptr);  // a failed add leaves the kernel as it was

  // A size that is not whole words fails in ShaderModule, before Vulkan.
  const Status words = builder.add(kernel, "truncated", vkc_test_add_comp_spv,
                                   vkc_test_add_comp_spv_size - 1, 1);
  EXPECT_TRUE(is_invalid(words));
  EXPECT_NE(words.message().find("truncated"), std::string::npos);

  // Nothing was registered, so build() hands out no set.
  const Result<DescriptorPool> pool = builder.build();
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  EXPECT_FALSE(kernel.valid());
}

TEST_F(ComputeTest, BuildsAKernelAgainWholeOrNotAtAll) {
  const VkPushConstantRange push = range(sizeof(AddPush));
  ComputeKernel kernel;
  KernelSetBuilder first(device());
  ASSERT_TRUE(first
                  .add(kernel, "test_add", vkc_test_add_comp_spv,
                       vkc_test_add_comp_spv_size, 1, &push)
                  .ok());
  Result<DescriptorPool> pool = first.build();
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  ASSERT_TRUE(kernel.valid());
  VkPipeline built = kernel.pipeline.handle();
  VkDescriptorSet set = kernel.set.handle();

  // A failed rebuild keeps the kernel whole: its pipeline, layout and set.
  KernelSetBuilder failing(device());
  EXPECT_TRUE(is_invalid(failing.add(kernel, "truncated", vkc_test_add_comp_spv,
                                     vkc_test_add_comp_spv_size - 1, 1)));
  EXPECT_TRUE(kernel.valid());
  EXPECT_EQ(kernel.pipeline.handle(), built);
  EXPECT_EQ(kernel.set.handle(), set);
  EXPECT_STREQ(kernel.name, "test_add");

  // A rebuild replaces it whole: no set of the old layout until build().
  KernelSetBuilder again(device());
  ASSERT_TRUE(again
                  .add(kernel, "test_fill", vkc_test_fill_comp_spv,
                       vkc_test_fill_comp_spv_size, 1, &push)
                  .ok());
  EXPECT_FALSE(kernel.valid());
  EXPECT_EQ(kernel.set.handle(), VK_NULL_HANDLE);
  EXPECT_STREQ(kernel.name, "test_fill");
  pool = again.build();
  ASSERT_TRUE(pool.ok()) << pool.status().message();
  EXPECT_TRUE(kernel.valid());
}

TEST_F(ComputeTest, KernelSetsGrowOnlyAndMove) {
  const VkPushConstantRange push = range(sizeof(AddPush));
  ComputeKernel add;
  KernelSetBuilder builder(device());
  ASSERT_TRUE(builder
                  .add(add, "test_add", vkc_test_add_comp_spv,
                       vkc_test_add_comp_spv_size, 1, &push)
                  .ok());
  const Result<DescriptorPool> pool = builder.build();
  ASSERT_TRUE(pool.ok());

  KernelSets sets;
  EXPECT_TRUE(is_invalid(sets.reserve(device(), add, 0)));
  EXPECT_TRUE(is_invalid(sets.reserve(device(), ComputeKernel{}, 1)));
  ASSERT_TRUE(sets.reserve(device(), add, 2).ok());
  EXPECT_EQ(sets.size(), 2U);
  VkDescriptorSet first = sets[0].handle();
  ASSERT_TRUE(sets.reserve(device(), add, 1).ok());  // fewer: kept
  EXPECT_EQ(sets.size(), 2U);
  EXPECT_EQ(sets[0].handle(), first);
  const DescriptorSet held = sets[0];
  ASSERT_TRUE(sets.reserve(device(), add, 3).ok());  // more: replaced
  EXPECT_EQ(sets.size(), 3U);
  // The old pool, and with it every copy of an old set, is gone.
  EXPECT_FALSE(held.valid());
  EXPECT_EQ(held.handle(), VK_NULL_HANDLE);

  KernelSets moved(std::move(sets));
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(sets.size(), 0U);
  EXPECT_EQ(moved.size(), 3U);
  KernelSets other;
  ASSERT_TRUE(other.reserve(device(), add, 1).ok());
  VkDescriptorSet kept = moved[0].handle();
  other = std::move(moved);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(moved.size(), 0U);
  EXPECT_EQ(other.size(), 3U);
  EXPECT_EQ(other[0].handle(), kept);
  KernelSets* self = &other;
  other = std::move(*self);
  EXPECT_EQ(other.size(), 3U);
  EXPECT_EQ(other[0].handle(), kept);
}

TEST_F(ComputeTest, DispatchesOnceAndRefusesWhatABatchRefuses) {
  constexpr std::uint32_t kCount = 300;
  const VkPushConstantRange push_range = range(4);
  ComputeKernel fill;
  KernelSetBuilder builder(device());
  ASSERT_TRUE(builder
                  .add(fill, "test_fill", vkc_test_fill_comp_spv,
                       vkc_test_fill_comp_spv_size, 1, &push_range)
                  .ok());
  const Result<DescriptorPool> pool = builder.build();
  ASSERT_TRUE(pool.ok());
  Result<Buffer> made =
      device_storage_buffer(allocator(), VkDeviceSize{kCount} * 4);
  ASSERT_TRUE(made.ok());
  const Buffer out = *std::move(made);
  fill.set.write_storage_buffer(0, out.handle(), 0, VK_WHOLE_SIZE);

  const std::uint32_t count = kCount;
  const Status s = dispatch(device(), fill, &count, sizeof(count),
                            group_count(kCount, 64), max_groups());
  ASSERT_TRUE(s.ok()) << s.message();
  const std::vector<std::uint32_t> values = read(out, kCount);
  for (std::uint32_t i = 0; i < kCount; ++i) ASSERT_EQ(values[i], i);

  EXPECT_TRUE(
      is_invalid(dispatch(device(), fill, nullptr, 4, 1, max_groups())));
  EXPECT_TRUE(is_invalid(dispatch(device(), fill, &count, 8, 1, max_groups())));
  EXPECT_TRUE(is_invalid(dispatch(device(), fill, &count, 2, 1, max_groups())));
  EXPECT_TRUE(is_invalid(dispatch(device(), fill, &count, 4, 2, 1)));
  EXPECT_TRUE(is_invalid(
      dispatch(device(), ComputeKernel{}, &count, 4, 1, max_groups())));
}

TEST_F(ComputeTest, MakesStorageBuffersWhereTheyBelong) {
  EXPECT_EQ(max_storage_buffer_range(device()),
            physical().limits().maxStorageBufferRange);

  Result<Buffer> host =
      mapped_storage_buffer(allocator(), 64, HostAccess::SequentialWrite,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  ASSERT_TRUE(host.ok()) << host.status().message();
  EXPECT_NE(host->mapped(), nullptr);
  EXPECT_TRUE(host->is_device_local());  // the GPU reads it at VRAM speed
  EXPECT_NE(host->usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0U);
  EXPECT_NE(host->usage() & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0U);

  const std::uint32_t words[4] = {1, 2, 3, 4};
  Result<Buffer> filled = upload_storage_buffer(allocator(), words, 16);
  ASSERT_TRUE(filled.ok()) << filled.status().message();
  EXPECT_TRUE(filled->is_device_local());
  EXPECT_EQ(static_cast<const std::uint32_t*>(filled->mapped())[3], 4U);
  EXPECT_TRUE(
      is_invalid(upload_storage_buffer(allocator(), nullptr, 16).status()));

  Result<Buffer> resident = device_storage_buffer(allocator(), 64);
  ASSERT_TRUE(resident.ok()) << resident.status().message();
  EXPECT_EQ(resident->mapped(), nullptr);
  EXPECT_TRUE(resident->is_device_local());
  // Kernel memory is private wherever the device has private memory for it.
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(device().handle(), resident->handle(), &needs);
  const VkPhysicalDeviceMemoryProperties& memory =
      physical().memory_properties();
  bool has_private = false;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
    has_private =
        has_private || ((needs.memoryTypeBits & (1U << i)) != 0 &&
                        (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                        (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                  VK_MEMORY_PROPERTY_PROTECTED_BIT)) == 0);
  }
  const std::optional<MemoryInfo> placed = resident->memory_info();
  if (!placed.has_value()) {
    FAIL() << "no memory info";
  }
  if (has_private) {
    EXPECT_EQ(placed->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0U);
  }
  const VkBufferUsageFlags transfer =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  EXPECT_EQ(resident->usage() & transfer, transfer);
}

TEST_F(ComputeTest, ScratchGrowsWithHeadroomWithinTheLimit) {
  Buffer scratch;
  ASSERT_TRUE(ensure_device_scratch(device(), allocator(), scratch, 1000, 4096,
                                    "test.scratch")
                  .ok());
  EXPECT_EQ(scratch.size(), 1500U);  // 1.5x
  VkBuffer kept = scratch.handle();
  ASSERT_TRUE(ensure_device_scratch(device(), allocator(), scratch, 1400, 4096,
                                    "test.scratch")
                  .ok());
  EXPECT_EQ(scratch.handle(), kept);  // fits: kept
  ASSERT_TRUE(ensure_device_scratch(device(), allocator(), scratch, 3000, 4096,
                                    "test.scratch")
                  .ok());
  EXPECT_EQ(scratch.size(), 4096U);  // headroom capped at the limit
  const Status over = ensure_device_scratch(device(), allocator(), scratch,
                                            5000, 4096, "test.scratch");
  EXPECT_TRUE(is_invalid(over));
  EXPECT_NE(over.message().find("test.scratch"), std::string::npos);
}

TEST_F(ComputeTest, ScratchGrownMidBatchStaysTheBatchsUntilItRuns) {
  // Written and copied out by a batch, then grown with that batch passed: the
  // copy still reads the old buffer, which the batch kept.
  const std::vector<std::uint32_t> words = {1, 2, 3, 4};
  Result<Buffer> made = device_storage_buffer(allocator(), 16);
  ASSERT_TRUE(made.ok());
  const Buffer out = *std::move(made);
  Buffer scratch;
  ASSERT_TRUE(ensure_device_scratch(device(), allocator(), scratch, 16, 4096,
                                    "test.scratch")
                  .ok());
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(scratch, 0, words.data(), 16).ok());
  ASSERT_TRUE(batch.copy(scratch, 0, out, 0, 16).ok());
  VkBuffer old = scratch.handle();
  ASSERT_TRUE(ensure_device_scratch(device(), allocator(), scratch, 1000, 4096,
                                    "test.scratch", &batch)
                  .ok());
  EXPECT_NE(scratch.handle(), old);
  EXPECT_EQ(scratch.size(), 1500U);
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(read(out, 4), words);
}

TEST_F(ComputeTest, StorageInputBindsDeviceBuffersAndStagesHostBytes) {
  Result<Buffer> resident = device_storage_buffer(allocator(), 64);
  Result<Buffer> mapped = mapped_storage_buffer(allocator(), 64);
  BufferDesc bare_desc;
  bare_desc.size = 64;
  bare_desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bare_desc.memory = MemoryUsage::DeviceOnly;
  Result<Buffer> bare = allocator().create_buffer(bare_desc);
  ASSERT_TRUE(resident.ok() && mapped.ok() && bare.ok());

  EXPECT_TRUE(StorageInput(*resident).check("t", 64).ok());
  EXPECT_TRUE(StorageInput(*mapped).check("t", 64).ok());  // zero-copy input
  EXPECT_TRUE(is_invalid(StorageInput(*resident).check("t", 65)));
  EXPECT_TRUE(is_invalid(StorageInput(*bare).check("t", 4)));  // no STORAGE
  // A borrowed buffer in host memory, or whose memory nobody recorded, is
  // refused: a kernel would read it across PCIe on a discrete GPU.
  const MemoryInfo host_memory{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               0, 0};
  const Buffer in_host(resident->handle(), 64,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_SHARING_MODE_EXCLUSIVE, nullptr, {}, host_memory);
  const Buffer unknown(resident->handle(), 64,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_SHARING_MODE_EXCLUSIVE, nullptr, {}, std::nullopt);
  EXPECT_TRUE(is_invalid(StorageInput(in_host).check("t", 4)));
  EXPECT_TRUE(is_invalid(StorageInput(unknown).check("t", 4)));

  Buffer upload;
  CommandBatch bind(device(), allocator());
  Result<VkBuffer> bound =
      StorageInput(*resident).buffer(bind, allocator(), 64, upload);
  ASSERT_TRUE(bound.ok());
  EXPECT_EQ(*bound, resident->handle());
  EXPECT_FALSE(upload.valid());  // a device input stages nothing

  const std::vector<std::uint32_t> words = {7, 8, 9, 10};
  CommandBatch stage(device(), allocator());
  Result<VkBuffer> staged =
      StorageInput(words.data()).buffer(stage, allocator(), 16, upload);
  ASSERT_TRUE(staged.ok()) << staged.status().message();
  EXPECT_EQ(*staged, upload.handle());
  ASSERT_TRUE(stage.submit().ok());
  EXPECT_EQ(read(upload, 4), words);
  VkBuffer reused = upload.handle();
  CommandBatch again(device(), allocator());
  ASSERT_TRUE(
      StorageInput(words.data()).buffer(again, allocator(), 8, upload).ok());
  EXPECT_EQ(upload.handle(), reused);  // big enough: reused
  ASSERT_TRUE(again.submit().ok());
}

TEST_F(ComputeTest, StorageInputKeepsAnUploadItOutgrowsForTheBatch) {
  // Two inputs through one upload member in one batch, the second larger:
  // what the batch recorded on the first buffer still runs on it.
  const std::vector<std::uint32_t> small = {1, 2, 3, 4};
  const std::vector<std::uint32_t> large = {5, 6, 7, 8, 9, 10, 11, 12};
  Result<Buffer> made = device_storage_buffer(allocator(), 16);
  ASSERT_TRUE(made.ok());
  const Buffer out = *std::move(made);
  Buffer upload;
  CommandBatch batch(device(), allocator());
  const Result<VkBuffer> first =
      StorageInput(small.data()).buffer(batch, allocator(), 16, upload);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(batch.copy(upload, 0, out, 0, 16).ok());
  const Result<VkBuffer> second =
      StorageInput(large.data()).buffer(batch, allocator(), 32, upload);
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_NE(*second, *first);  // outgrown: replaced
  EXPECT_EQ(upload.handle(), *second);
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(read(out, 4), small);
  EXPECT_EQ(read(upload, 8), large);
}

}  // namespace
}  // namespace volumetric_kit::core
