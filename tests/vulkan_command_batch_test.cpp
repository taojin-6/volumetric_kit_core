// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// CommandBatch, ported from recon's core_command_batch_test: every recording
// call, in orders that would expose a command run out of place -- an upload
// after a dispatch, a readback before one -- over a device-only and a
// device-mapped buffer, as a buffer's memory type must not change what a batch
// does. Uploads inline, staged and packed by the caller, several readbacks in
// one batch, transfers left unordered (fills, uploads and copies rising
// through one buffer among them), image copies, acquires and releases,
// indirect dispatch, rewritten sets, the refusals, the moves, and several
// threads at once.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "add_comp.spv.hpp"
#include "batch_visibility_frag.spv.hpp"
#include "batch_visibility_vert.spv.hpp"
#include "command_batch_barriers.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_kernel.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shader.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "vulkan_device_fixture.hpp"
#include "vulkan_fixture.hpp"

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#include <stdexcept>
#define VKC_TEST_EXCEPTIONS 1
#endif

namespace volumetric_kit::core {
namespace {

constexpr std::uint32_t kCount = 256;  // four workgroups of add.comp
constexpr VkDeviceSize kBytes = kCount * sizeof(std::uint32_t);

struct Push {
  std::uint32_t count;
  std::uint32_t delta;
};

std::vector<std::uint32_t> pattern(std::uint32_t base, std::size_t n = kCount) {
  std::vector<std::uint32_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = base + (static_cast<std::uint32_t>(i) * 7U);
  }
  return v;
}

std::vector<std::uint32_t> plus(std::vector<std::uint32_t> v,
                                std::uint32_t delta) {
  for (auto& x : v) x += delta;
  return v;
}

bool is_invalid(const Status& s) {
  return s.domain() == Status::Code::InvalidArgument;
}

// An image handle nothing was made for, for refusals that never reach Vulkan.
VkImage fake_image() {
  static_assert(sizeof(VkImage) == sizeof(std::uintptr_t), "a pointer handle");
  // NOLINTNEXTLINE(performance-no-int-to-ptr,*-reinterpret-cast)
  return reinterpret_cast<VkImage>(std::uintptr_t{0x1000});
}

Image fake(VkFormat format, std::uint32_t width, std::uint32_t height,
           VkImageLayout layout,
           VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
  ImageInfo info;
  info.image = fake_image();
  info.format = format;
  info.extent = {width, height, 1};
  info.samples = samples;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  info.layout = layout;
  return {info, {}};
}

class BatchTest : public test::VulkanDeviceTest {
 protected:
  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    max_groups_ = physical().limits().maxComputeWorkGroupCount[0];
    range_.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range_.size = sizeof(Push);
    KernelSetBuilder builder(device());
    ASSERT_TRUE(builder
                    .add(add_, "test_add", vkc_test_add_comp_spv,
                         vkc_test_add_comp_spv_size, 1, &range_)
                    .ok());
    Result<DescriptorPool> pool = builder.build();
    ASSERT_TRUE(pool.ok()) << pool.status().message();
    pool_ = *std::move(pool);
    Result<Buffer> a = device_storage_buffer(allocator(), kBytes);
    Result<Buffer> b = device_storage_buffer(allocator(), kBytes);
    ASSERT_TRUE(a.ok() && b.ok());
    a_ = *std::move(a);
    b_ = *std::move(b);
  }

  void TearDown() override {
    a_ = Buffer{};
    b_ = Buffer{};
    add_ = ComputeKernel{};
    pool_ = DescriptorPool{};
    VulkanDeviceTest::TearDown();
  }

  // Bind `buffer` to the add kernel and record it over the first kCount
  // values.
  Status add_to(CommandBatch& batch, const Buffer& buffer, std::uint32_t delta,
                const ComputeKernel* kernel = nullptr) {
    const ComputeKernel& k = kernel != nullptr ? *kernel : add_;
    k.set.write_storage_buffer(0, buffer.handle(), 0, VK_WHOLE_SIZE);
    const Push push{kCount, delta};
    return batch.dispatch(k, &push, sizeof(push), group_count(kCount, 64),
                          max_groups_);
  }

  // A device-only buffer, or a device-mapped one, with every usage a batch
  // call needs: both device-local, as everything a kernel binds is.
  Buffer make(bool device_only, VkDeviceSize bytes) {
    Result<Buffer> made =
        device_only ? device_storage_buffer(allocator(), bytes)
                    : mapped_storage_buffer(
                          allocator(), bytes, HostAccess::SequentialWrite,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    EXPECT_TRUE(made.ok()) << made.status().message();
    return made.ok() ? *std::move(made) : Buffer{};
  }

  void upload(const Buffer& buffer, const std::vector<std::uint32_t>& words) {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(buffer, 0, words.data(), words.size() * 4).ok());
    ASSERT_TRUE(batch.submit().ok());
  }

  std::vector<std::uint32_t> read(const Buffer& buffer,
                                  std::size_t count = kCount) {
    std::vector<std::uint32_t> out(count, 0xDEADBEEFU);
    CommandBatch batch(device(), allocator());
    EXPECT_TRUE(batch.readback(buffer, 0, count * 4, out.data()).ok());
    EXPECT_TRUE(batch.submit().ok());
    return out;
  }

  // A `width` x `height` image of `format` holding `texels`, rows packed, left
  // in GENERAL, as its writer would leave it for a copy.
  Image make_image(VkFormat format, std::uint32_t width, std::uint32_t height,
                   const std::vector<std::uint8_t>& texels,
                   VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
    ImageDesc desc;
    desc.extent = {width, height};
    desc.format = format;
    desc.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.with_view = false;
    Result<Image> made = allocator().create_image(desc);
    EXPECT_TRUE(made.ok()) << made.status().message();
    if (!made.ok()) return Image{};
    Image image = *std::move(made);
    BufferDesc staging_desc;
    staging_desc.size = texels.size();
    staging_desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_desc.memory = MemoryUsage::Staging;
    Result<Buffer> staging = allocator().create_buffer(staging_desc);
    EXPECT_TRUE(staging.ok());
    if (!staging.ok()) return Image{};
    std::memcpy(staging->mapped(), texels.data(), texels.size());
    VkImage raw = image.handle();
    const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = raw;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {width, height, 1};
      vkCmdCopyBufferToImage(cmd, staging->handle(), raw,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    });
    EXPECT_TRUE(s.ok()) << s.message();
    image.set_layout(VK_IMAGE_LAYOUT_GENERAL);
    return image;
  }

  // Whether `record` is refused, and then poisons the batch's submit.
  template <typename Record>
  bool refused(Record&& record) {
    CommandBatch batch(device(), allocator());
    const Status first = record(batch);
    return is_invalid(first) && is_invalid(batch.submit());
  }

  std::uint32_t max_groups_ = 0;
  VkPushConstantRange range_{};
  ComputeKernel add_;
  DescriptorPool pool_;
  Buffer a_;
  Buffer b_;
};

TEST(BatchBarriers, TransferOnlyScopesNeedNoShaderOrDrawSupport) {
  const detail::BatchScope writes = detail::batch_writes(VK_QUEUE_TRANSFER_BIT);
  const detail::BatchScope commands =
      detail::batch_commands(VK_QUEUE_TRANSFER_BIT);
  const detail::BatchScope consumers =
      detail::batch_consumers(VK_QUEUE_TRANSFER_BIT);
  EXPECT_EQ(writes.stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(commands.stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(consumers.stages,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT);
  EXPECT_EQ(writes.access, VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(commands.access,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(consumers.access, commands.access | VK_ACCESS_HOST_READ_BIT);
}

TEST(BatchBarriers, GraphicsAndComputeScopesFollowTheirQueueCapabilities) {
  const detail::BatchScope graphics_writes =
      detail::batch_writes(VK_QUEUE_GRAPHICS_BIT);
  const detail::BatchScope graphics_commands =
      detail::batch_commands(VK_QUEUE_GRAPHICS_BIT);
  const detail::BatchScope graphics_consumers =
      detail::batch_consumers(VK_QUEUE_GRAPHICS_BIT);
  EXPECT_EQ((graphics_writes.stages | graphics_commands.stages |
             graphics_consumers.stages) &
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0U);
  EXPECT_NE(graphics_consumers.stages & VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0U);
  EXPECT_NE(graphics_consumers.access & VK_ACCESS_UNIFORM_READ_BIT, 0U);
  const detail::BatchScope compute_consumers =
      detail::batch_consumers(VK_QUEUE_COMPUTE_BIT);
  EXPECT_EQ(compute_consumers.stages & (VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT),
            0U);
  EXPECT_NE(compute_consumers.stages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0U);
}

// Exercises the final dependency against actual graphics-stage descriptor
// reads. The barrier mode records the batch's final barrier immediately
// before a draw: waiting on a fence between submissions retires validation's
// access history, so testing only the public blocking submit would miss this
// hazard. The other mode also checks the complete public upload-to-render
// path.
//
// TODO: neither mode proves CommandBatch::record emits that barrier, which
// only a private recorder can show in one submission with a draw. Cover it
// once the batch can record into a caller's command buffer, rather than
// opening a friend backdoor (AGENTS.md).
class BatchGraphicsTest : public test::VulkanDeviceTest,
                          public ::testing::WithParamInterface<bool> {
 protected:
  DeviceRequirements requirements() const override {
    DeviceRequirements req;
    req.api_version = VK_API_VERSION_1_3;
    req.queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    req.dynamic_rendering = true;
    return req;
  }

  Status render_uploaded_descriptors(bool public_batch,
                                     std::array<std::uint8_t, 4>& pixel) {
    constexpr std::array<std::uint32_t, 4> kValues{17, 34, 68, 102};
    std::array<Buffer, 4> inputs;
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
      const bool uniform = i % 2 == 0;
      BufferDesc desc;
      desc.size = 16;
      desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   (uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                            : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      VKC_ASSIGN(inputs[i], allocator().create_buffer(desc));
      bindings[i].binding = i;
      bindings[i].descriptorType = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags =
          i < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VKC_ASSIGN(const DescriptorSetLayout layout,
               DescriptorSetLayout::create(device().handle(), bindings.data(),
                                           bindings.size()));
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VKC_ASSIGN(DescriptorPool pool,
               DescriptorPool::create(device().handle(), sizes, 2, 1));
    VKC_ASSIGN(DescriptorSet set, pool.allocate(layout.handle()));
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
      if (i % 2 == 0) {
        set.write_uniform_buffer(i, inputs[i].handle(), 0, inputs[i].size());
      } else {
        set.write_storage_buffer(i, inputs[i].handle(), 0, inputs[i].size());
      }
    }
    std::vector<std::uint32_t> vertex_code(
        vkc_test_batch_visibility_vert_spv_size / sizeof(std::uint32_t));
    std::vector<std::uint32_t> fragment_code(
        vkc_test_batch_visibility_frag_spv_size / sizeof(std::uint32_t));
    std::memcpy(vertex_code.data(), vkc_test_batch_visibility_vert_spv,
                vkc_test_batch_visibility_vert_spv_size);
    std::memcpy(fragment_code.data(), vkc_test_batch_visibility_frag_spv,
                vkc_test_batch_visibility_frag_spv_size);
    VKC_ASSIGN(const ShaderModule vertex,
               ShaderModule::create(device().handle(), vertex_code.data(),
                                    vkc_test_batch_visibility_vert_spv_size));
    VKC_ASSIGN(const ShaderModule fragment,
               ShaderModule::create(device().handle(), fragment_code.data(),
                                    vkc_test_batch_visibility_frag_spv_size));
    VkDescriptorSetLayout raw_layout = layout.handle();
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &raw_layout;
    VkPipelineLayout raw_pipeline_layout = VK_NULL_HANDLE;
    VKC_VK_TRY(vkCreatePipelineLayout(device().handle(), &layout_info, nullptr,
                                      &raw_pipeline_layout));
    const UniqueHandle<VkPipelineLayout, vkDestroyPipelineLayout>
        pipeline_layout(device().handle(), raw_pipeline_layout);
    VKC_ASSIGN(auto pipeline,
               graphics_pipeline(vertex, fragment, raw_pipeline_layout));

    ImageDesc image_desc;
    image_desc.extent = {1, 1};
    image_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_desc.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VKC_ASSIGN(Image target, allocator().create_image(image_desc));
    BufferDesc readback_desc;
    readback_desc.size = pixel.size();
    readback_desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readback_desc.memory = MemoryUsage::Staging;
    readback_desc.host_access = HostAccess::Random;
    VKC_ASSIGN(Buffer readback, allocator().create_buffer(readback_desc));
    if (public_batch) {
      CommandBatch batch(device(), allocator());
      for (std::uint32_t i = 0; i < inputs.size(); ++i) {
        VKC_TRY(batch.upload(inputs[i], 0, &kValues[i], sizeof(kValues[i])));
      }
      VKC_TRY(batch.submit());
    }
    VKC_TRY(device().submit_single_time([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier transition{};
      transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      transition.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      transition.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      transition.image = target.handle();
      transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &transition);
      if (!public_batch) {
        for (std::uint32_t i = 0; i < inputs.size(); ++i) {
          vkCmdUpdateBuffer(cmd, inputs[i].handle(), 0, sizeof(kValues[i]),
                            &kValues[i]);
        }
        detail::batch_final_barrier(cmd, device().queue_flags());
      }
      VkRenderingAttachmentInfo color{};
      color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color.imageView = target.view();
      color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo rendering{};
      rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      rendering.renderArea.extent = {1, 1};
      rendering.layerCount = 1;
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachments = &color;
      vkCmdBeginRendering(cmd, &rendering);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get());
      const auto raw_set = set.handle();
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              raw_pipeline_layout, 0, 1, &raw_set, 0, nullptr);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRendering(cmd);
      transition.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      transition.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &transition);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {1, 1, 1};
      vkCmdCopyImageToBuffer(cmd, target.handle(),
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.handle(), 1, &region);
      test::host_read_barrier(cmd);
    }));
    std::memcpy(pixel.data(), readback.mapped(), pixel.size());
    return {};
  }

  Result<UniqueHandle<VkPipeline, vkDestroyPipeline>> graphics_pipeline(
      const ShaderModule& vertex, const ShaderModule& fragment,
      VkPipelineLayout layout) {
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex.handle(), "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment.handle(), "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, 1, 1, 0, 1};
    const VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewports{};
    viewports.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewports.viewportCount = 1;
    viewports.pViewports = &viewport;
    viewports.scissorCount = 1;
    viewports.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    const VkPipelineMultisampleStateCreateInfo samples{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,
        0,
        nullptr,
        VK_FALSE,
        VK_FALSE};
    VkPipelineColorBlendAttachmentState color{};
    color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewports;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pColorBlendState = &blend;
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VKC_VK_TRY(vkCreateGraphicsPipelines(device().handle(), VK_NULL_HANDLE, 1,
                                         &info, nullptr, &pipeline));
    return UniqueHandle<VkPipeline, vkDestroyPipeline>(device().handle(),
                                                       pipeline);
  }
};

TEST_P(BatchGraphicsTest, UploadsAreVisibleToGraphicsDescriptors) {
  std::array<std::uint8_t, 4> pixel{};
  const Status rendered = render_uploaded_descriptors(GetParam(), pixel);
  ASSERT_TRUE(rendered.ok()) << rendered.message();
  EXPECT_EQ(pixel, (std::array<std::uint8_t, 4>{17, 34, 68, 102}));
}

INSTANTIATE_TEST_SUITE_P(FinalBarrierAndPublicBatch, BatchGraphicsTest,
                         ::testing::Bool());

// The visibility tests above check nothing without synchronization
// validation, which the layer runs only when VK_LAYER_ENABLES asks for it.
// Under VKC_TEST_SYNC_VALIDATION=1, a deliberate hazard must be reported.
using SyncValidationTest = test::VulkanDeviceTest;

TEST_F(SyncValidationTest, ReportsADeliberateHazard) {
  if (!test::env_set("VKC_TEST_SYNC_VALIDATION")) {
    GTEST_SKIP() << "VKC_TEST_SYNC_VALIDATION is not set";
  }
  ASSERT_TRUE(test::env_set("VKC_TEST_VALIDATION"))
      << "VKC_TEST_SYNC_VALIDATION needs VKC_TEST_VALIDATION";
  BufferDesc desc;
  desc.size = 64;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Result<Buffer> made = allocator().create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer buffer = *std::move(made);
  allow_validation_errors();
  // Two writes of the same bytes with no barrier between: write after write,
  // which only synchronization validation reports.
  const Status submitted =
      device().submit_single_time([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 1);
        vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 2);
      });
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  EXPECT_GT(allowed_validation_errors(), 0)
      << "synchronization validation is off: set VK_LAYER_ENABLES="
         "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT";
}

// --- in order, over both memory kinds ----------------------------------------

class BatchOrderTest : public BatchTest,
                       public ::testing::WithParamInterface<bool> {};

TEST_P(BatchOrderTest, RunsCommandsInTheOrderRecorded) {
  if (!GetParam() && !physical().device_mapped_memory()) {
    GTEST_SKIP() << "the device has no device-mapped memory";
  }
  const Buffer buffer = make(GetParam(), kBytes);
  ASSERT_TRUE(buffer.valid());

  // Round trip, inline (1 KiB, aligned).
  const std::vector<std::uint32_t> p = pattern(100);
  std::vector<std::uint32_t> got(kCount, 0);
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(buffer, 0, p.data(), kBytes).ok());
    ASSERT_TRUE(batch.readback(buffer, 0, kBytes, got.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(got, p);

  // Upload, dispatch, readback: one submit, in order.
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(buffer, 0, p.data(), kBytes).ok());
    ASSERT_TRUE(add_to(batch, buffer, 5).ok());
    ASSERT_TRUE(batch.readback(buffer, 0, kBytes, got.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(got, plus(p, 5));

  // An upload recorded after a dispatch lands after it.
  const std::vector<std::uint32_t> q = pattern(9000);
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(add_to(batch, buffer, 1).ok());
    ASSERT_TRUE(batch.upload(buffer, 0, q.data(), kBytes).ok());
    ASSERT_TRUE(batch.readback(buffer, 0, kBytes, got.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(got, q);

  // A readback recorded before a dispatch reads what stood before it.
  std::vector<std::uint32_t> before(kCount, 0);
  std::vector<std::uint32_t> after(kCount, 0);
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.readback(buffer, 0, kBytes, before.data()).ok());
    ASSERT_TRUE(add_to(batch, buffer, 3).ok());
    ASSERT_TRUE(batch.readback(buffer, 0, kBytes, after.data()).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(before, q);
  EXPECT_EQ(after, plus(q, 3));

  // Offsets: a fill, an inline partial upload and a staged unaligned one,
  // then several small readbacks at odd sizes out of one batch.
  const unsigned char odd[6] = {1, 2, 3, 4, 5, 6};
  std::uint32_t head[3] = {};
  unsigned char middle[6] = {};
  std::uint32_t tail = 0;
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.fill(buffer, 0, kBytes, 0xABCDU).ok());
    ASSERT_TRUE(batch.upload(buffer, 16, p.data(), 32).ok());  // inline
    ASSERT_TRUE(batch.upload(buffer, 102, odd, 6).ok());       // staged
    ASSERT_TRUE(batch.readback(buffer, 12, 12, head).ok());
    ASSERT_TRUE(batch.readback(buffer, 102, 6, middle).ok());
    ASSERT_TRUE(batch.readback(buffer, kBytes - 4, 4, &tail).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_EQ(head[0], 0xABCDU);
  EXPECT_EQ(head[1], p[0]);
  EXPECT_EQ(head[2], p[1]);
  for (int i = 0; i < 6; ++i) EXPECT_EQ(middle[i], odd[i]);
  EXPECT_EQ(tail, 0xABCDU);

  // A reserved upload, filled by the caller after the call, at an odd offset.
  unsigned char packed[10] = {};
  {
    CommandBatch batch(device(), allocator());
    Result<void*> staging = batch.reserve_upload(buffer, 41, 10);
    ASSERT_TRUE(staging.ok());
    ASSERT_NE(*staging, nullptr);
    for (int i = 0; i < 10; ++i) {
      static_cast<unsigned char*>(*staging)[i] =
          static_cast<unsigned char>(200 + i);
    }
    ASSERT_TRUE(batch.readback(buffer, 41, 10, packed).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  for (int i = 0; i < 10; ++i) EXPECT_EQ(packed[i], 200 + i);
}

INSTANTIATE_TEST_SUITE_P(Memory, BatchOrderTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "DeviceOnly" : "DeviceMapped";
                         });

// --- transfers ---------------------------------------------------------------

TEST_F(BatchTest, StagesAnUploadPastTheInlineLimit) {
  const std::size_t n = (CommandBatch::kMaxInlineUpload * 2) / 4;
  const Buffer big = make(true, CommandBatch::kMaxInlineUpload * 2);
  const std::vector<std::uint32_t> data = pattern(77, n);
  std::vector<std::uint32_t> back(n, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(big, 0, data.data(), big.size()).ok());
  ASSERT_TRUE(batch.readback(big, 0, big.size(), back.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(back, data);
}

TEST_F(BatchTest, LeavesTransfersOnDifferentBuffersUnordered) {
  const std::vector<std::uint32_t> p = pattern(1);
  const std::vector<std::uint32_t> q = pattern(500);
  std::vector<std::uint32_t> got_a(kCount, 0);
  std::vector<std::uint32_t> got_b(kCount, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(a_, 0, p.data(), kBytes).ok());
  ASSERT_TRUE(batch.upload(b_, 0, q.data(), kBytes).ok());
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, got_a.data()).ok());
  ASSERT_TRUE(batch.readback(b_, 0, kBytes, got_b.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(got_a, p);
  EXPECT_EQ(got_b, q);
}

TEST_F(BatchTest, CopiesBetweenAndWithinBuffers) {
  const std::vector<std::uint32_t> p = pattern(1);
  std::vector<std::uint32_t> got(kCount, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(a_, 0, p.data(), kBytes).ok());
  ASSERT_TRUE(batch.copy(a_, 0, b_, 0, kBytes).ok());
  ASSERT_TRUE(batch.copy(b_, 0, b_, kBytes / 2, kBytes / 2).ok());
  ASSERT_TRUE(batch.readback(b_, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  for (std::uint32_t i = 0; i < kCount / 2; ++i) {
    EXPECT_EQ(got[i], p[i]);
    EXPECT_EQ(got[(kCount / 2) + i], p[i]);
  }
}

TEST_F(BatchTest, ZeroesAtAnyAlignment) {
  // Inside one word, both edges off a word, aligned, ending on a word -- and
  // not a byte either side touched.
  const std::vector<std::uint32_t> ones(kCount, 0xFFFFFFFFU);
  std::vector<unsigned char> bytes(kBytes, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(a_, 0, ones.data(), kBytes).ok());
  ASSERT_TRUE(batch.zero(a_, 1, 2).ok());
  ASSERT_TRUE(batch.zero(a_, 7, 10).ok());
  ASSERT_TRUE(batch.zero(a_, 32, 8).ok());
  ASSERT_TRUE(batch.zero(a_, 45, 7).ok());
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, bytes.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  for (VkDeviceSize i = 0; i < kBytes; ++i) {
    const bool zeroed = (i >= 1 && i < 3) || (i >= 7 && i < 17) ||
                        (i >= 32 && i < 40) || (i >= 45 && i < 52);
    ASSERT_EQ(bytes[i], zeroed ? 0 : 0xFF) << "byte " << i;
  }
}

TEST_F(BatchTest, ZeroesManyScatteredUnalignedBlocks) {
  // Each block has two unaligned edges, all copied from the batch's one
  // staged word of zeros, and not a byte between the blocks touched.
  constexpr VkDeviceSize kBlocks = 60;
  const std::vector<std::uint32_t> ones(kCount, 0xFFFFFFFFU);
  std::vector<unsigned char> bytes(kBytes, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(a_, 0, ones.data(), kBytes).ok());
  for (VkDeviceSize b = 0; b < kBlocks; ++b) {
    ASSERT_TRUE(batch.zero(a_, (b * 16) + 1, 9).ok());
  }
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, bytes.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  for (VkDeviceSize i = 0; i < kBytes; ++i) {
    const VkDeviceSize at = i % 16;
    const bool zeroed = i < kBlocks * 16 && at >= 1 && at < 10;
    ASSERT_EQ(bytes[i], zeroed ? 0 : 0xFF) << "byte " << i;
  }
}

TEST_F(BatchTest, RetainsABufferItsCommandsUseUntilTheyRun) {
  // A buffer the caller lets go after recording on it: the batch keeps it.
  const std::vector<std::uint32_t> p = pattern(5);
  std::vector<std::uint32_t> got(kCount, 0);
  Buffer scratch = make(true, kBytes);
  ASSERT_TRUE(scratch.valid());
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(scratch, 0, p.data(), kBytes).ok());
  ASSERT_TRUE(batch.copy(scratch, 0, b_, 0, kBytes).ok());
  batch.retain(std::move(scratch));
  batch.retain(Buffer{});  // nothing to keep
  ASSERT_TRUE(batch.readback(b_, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(got, p);
}

TEST_F(BatchTest, RunsRisingFillsTogetherAndOrdersOneThatGoesBack) {
  std::vector<std::uint32_t> got(kCount, 0);
  CommandBatch batch(device(), allocator());
  for (std::uint32_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(batch.fill(a_, VkDeviceSize{i} * 4, 4, i).ok());
  }
  ASSERT_TRUE(batch.fill(a_, 0, 16, 7U).ok());
  ASSERT_TRUE(add_to(batch, a_, 1).ok());
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  for (std::uint32_t i = 0; i < kCount; ++i) {
    ASSERT_EQ(got[i], (i < 4 ? 7U : i) + 1) << "word " << i;
  }
}

TEST_F(BatchTest, RunsRisingStagedUploadsTogetherAndOrdersOneThatGoesBack) {
  const VkDeviceSize half = CommandBatch::kMaxInlineUpload * 2;
  const std::size_t n = half / 4;
  const Buffer big = make(true, half * 2);
  const std::vector<std::uint32_t> first = pattern(3, n);
  const std::vector<std::uint32_t> second = pattern(9, n);
  const std::vector<std::uint32_t> over = pattern(40, n);
  std::vector<std::uint32_t> back(2 * n, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(big, 0, first.data(), half).ok());
  ASSERT_TRUE(batch.upload(big, half, second.data(), half).ok());
  ASSERT_TRUE(batch.upload(big, half / 2, over.data(), half).ok());
  ASSERT_TRUE(batch.readback(big, 0, half * 2, back.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  const std::size_t lo = n / 2;
  for (std::size_t i = 0; i < 2 * n; ++i) {
    std::uint32_t want = 0;
    if (i < lo) {
      want = first[i];
    } else if (i < lo + n) {
      want = over[i - lo];
    } else {
      want = second[i - n];
    }
    ASSERT_EQ(back[i], want) << "word " << i;
  }
}

TEST_F(BatchTest, OrdersACopyWhoseSourceTheRunWrote) {
  const std::vector<std::uint32_t> p = pattern(1);
  const VkDeviceSize half = kBytes / 2;
  const Buffer c = make(true, kBytes);
  upload(a_, p);
  std::vector<std::uint32_t> got(kCount, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.fill(b_, 0, kBytes, 9U).ok());
  ASSERT_TRUE(batch.copy(a_, 0, c, 0, half / 2).ok());
  ASSERT_TRUE(batch.copy(a_, half / 2, c, half / 2, half / 2).ok());
  ASSERT_TRUE(batch.copy(b_, 0, c, half, half).ok());
  ASSERT_TRUE(batch.readback(c, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  for (std::uint32_t i = 0; i < kCount; ++i) {
    ASSERT_EQ(got[i], i < kCount / 2 ? p[i] : 9U) << "word " << i;
  }
}

// --- dispatches --------------------------------------------------------------

TEST_F(BatchTest, DispatchesIndirectlyFromABuffer) {
  // Two of four groups.
  const std::vector<std::uint32_t> p = pattern(1);
  Result<Buffer> made =
      device_storage_buffer(allocator(), sizeof(VkDispatchIndirectCommand),
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
  ASSERT_TRUE(made.ok());
  const Buffer args = *std::move(made);
  const VkDispatchIndirectCommand command{2, 1, 1};
  add_.set.write_storage_buffer(0, a_.handle(), 0, VK_WHOLE_SIZE);
  const Push push{kCount, 10};
  std::vector<std::uint32_t> got(kCount, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(a_, 0, p.data(), kBytes).ok());
  ASSERT_TRUE(batch.upload(args, 0, &command, sizeof(command)).ok());
  ASSERT_TRUE(batch.dispatch_indirect(add_, &push, sizeof(push), args, 0).ok());
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  for (std::uint32_t i = 0; i < kCount; ++i) {
    ASSERT_EQ(got[i], i < 128 ? p[i] + 10 : p[i]) << "word " << i;
  }
  // Misaligned, too short, and without INDIRECT_BUFFER usage.
  EXPECT_TRUE(refused([&](CommandBatch& c) {
    return c.dispatch_indirect(add_, &push, sizeof(push), args, 2);
  }));
  EXPECT_TRUE(refused([&](CommandBatch& c) {
    return c.dispatch_indirect(add_, &push, sizeof(push), args, 4);
  }));
  EXPECT_TRUE(refused([&](CommandBatch& c) {
    return c.dispatch_indirect(add_, &push, sizeof(push), a_, 0);
  }));
}

TEST_F(BatchTest, RefusesASetRewrittenAfterItsDispatch) {
  const std::vector<std::uint32_t> z(kCount, 0);
  upload(a_, z);
  upload(b_, z);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(add_to(batch, a_, 1).ok());
  ASSERT_TRUE(add_to(batch, b_, 2).ok());  // rewrites the kernel's set
  EXPECT_TRUE(is_invalid(batch.submit()));
  EXPECT_EQ(read(a_), z);  // neither dispatch ran
  EXPECT_EQ(read(b_), z);
}

TEST_F(BatchTest, RefusesASetRewrittenThroughACopy) {
  // Copies alias the Vulkan set: a write through any copy refuses both the
  // kernel's own binding and a supplied one.
  for (const bool explicit_set : {false, true}) {
    for (const bool copy_assign : {false, true}) {
      SCOPED_TRACE(std::string(explicit_set ? "explicit" : "own") + " set, " +
                   (copy_assign ? "copy-assigned" : "copy-constructed"));
      const std::vector<std::uint32_t> z(kCount, 0);
      upload(a_, z);
      upload(b_, z);
      add_.set.write_storage_buffer(0, a_.handle(), 0, kBytes);
      const DescriptorSet recorded = add_.set;
      CommandBatch batch(device(), allocator());
      const Push push{kCount, 7};
      if (explicit_set) {
        ASSERT_TRUE(
            batch.dispatch(add_, recorded, &push, sizeof(push), 4, max_groups_)
                .ok());
      } else {
        ASSERT_TRUE(
            batch.dispatch(add_, &push, sizeof(push), 4, max_groups_).ok());
      }
      DescriptorSet alias;
      if (copy_assign) {
        alias = recorded;
      } else {
        alias = DescriptorSet(recorded);
      }
      alias.write_storage_buffer(0, b_.handle(), 0, kBytes);
      EXPECT_TRUE(is_invalid(batch.submit()));
      EXPECT_EQ(read(a_), z);
      EXPECT_EQ(read(b_), z);
    }
  }
}

TEST_F(BatchTest, DispatchesOneKernelOverSetsOfItsOwn) {
  KernelSets sets;
  ASSERT_TRUE(sets.reserve(device(), add_, 2).ok());
  const DescriptorSet& sa = sets[0];
  const DescriptorSet& sb = sets[1];
  const std::vector<std::uint32_t> p = pattern(1);
  const std::vector<std::uint32_t> q = pattern(900);
  upload(a_, p);
  upload(b_, q);

  const Push one{kCount, 1};
  const Push two{kCount, 2};
  const std::uint32_t groups = group_count(kCount, 64);
  sa.write_storage_buffer(0, a_.handle(), 0, VK_WHOLE_SIZE);
  sb.write_storage_buffer(0, b_.handle(), 0, VK_WHOLE_SIZE);
  std::vector<std::uint32_t> got_a(kCount, 0);
  std::vector<std::uint32_t> got_b(kCount, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(
      batch.dispatch(add_, sa, &one, sizeof(one), groups, max_groups_).ok());
  ASSERT_TRUE(
      batch.dispatch(add_, sb, &two, sizeof(two), groups, max_groups_).ok());
  // The first set again, after the second: one buffer twice, in order.
  ASSERT_TRUE(
      batch.dispatch(add_, sa, &two, sizeof(two), groups, max_groups_).ok());
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, got_a.data()).ok());
  ASSERT_TRUE(batch.readback(b_, 0, kBytes, got_b.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(got_a, plus(p, 3));
  EXPECT_EQ(got_b, plus(q, 2));

  // The batch binds the set it was given, so the caller's object may be
  // reassigned, or be a temporary; a set rewritten after its dispatch is
  // refused.
  DescriptorSet swapped = sa;
  CommandBatch reassigned(device(), allocator());
  ASSERT_TRUE(
      reassigned.dispatch(add_, swapped, &one, sizeof(one), groups, max_groups_)
          .ok());
  ASSERT_TRUE(reassigned
                  .dispatch(add_, DescriptorSet(sb), &one, sizeof(one), groups,
                            max_groups_)
                  .ok());
  swapped = sb;
  ASSERT_TRUE(reassigned.submit().ok());
  EXPECT_EQ(read(a_), plus(p, 4));
  EXPECT_EQ(read(b_), plus(q, 3));

  CommandBatch rewritten(device(), allocator());
  ASSERT_TRUE(
      rewritten.dispatch(add_, sb, &one, sizeof(one), groups, max_groups_)
          .ok());
  sb.write_storage_buffer(0, a_.handle(), 0, VK_WHOLE_SIZE);
  EXPECT_TRUE(is_invalid(rewritten.submit()));

  const DescriptorSet none;
  CommandBatch empty_set(device(), allocator());
  EXPECT_TRUE(is_invalid(
      empty_set.dispatch(add_, none, &one, sizeof(one), groups, max_groups_)));
}

TEST_F(BatchTest, RefusesASetFreedWithItsPool) {
  // A KernelSets that grows frees the sets it held, and a kernel's pool can
  // go too: either way the recorded binding is gone, and submit refuses
  // rather than bind freed memory.
  const std::vector<std::uint32_t> z(kCount, 0);
  upload(a_, z);
  const Push push{kCount, 1};
  const std::uint32_t groups = group_count(kCount, 64);
  KernelSets sets;
  ASSERT_TRUE(sets.reserve(device(), add_, 1).ok());
  sets[0].write_storage_buffer(0, a_.handle(), 0, VK_WHOLE_SIZE);
  CommandBatch grown(device(), allocator());
  ASSERT_TRUE(
      grown.dispatch(add_, sets[0], &push, sizeof(push), groups, max_groups_)
          .ok());
  ASSERT_TRUE(sets.reserve(device(), add_, 2).ok());
  EXPECT_TRUE(is_invalid(grown.submit()));

  CommandBatch own(device(), allocator());
  ASSERT_TRUE(add_to(own, a_, 1).ok());
  pool_ = DescriptorPool{};
  EXPECT_TRUE(is_invalid(own.submit()));
  EXPECT_FALSE(add_.valid());  // its set went with the pool
  EXPECT_EQ(read(a_), z);      // neither dispatch ran
}

TEST_F(BatchTest, BindsTheKernelItRecordedThoughTheKernelMoves) {
  // The batch takes the kernel's pipeline and set when it records, so a
  // kernel moved before submit -- a vector of kernels that grew -- still runs.
  const std::vector<std::uint32_t> p = pattern(1);
  upload(a_, p);
  std::vector<std::uint32_t> got(kCount, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(add_to(batch, a_, 6).ok());
  ComputeKernel moved = std::move(add_);
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(got, plus(p, 6));
  add_ = std::move(moved);
}

// --- acquires ----------------------------------------------------------------

TEST_F(BatchTest, AcquiresABufferBeforeWhatReadsIt) {
  // From outside Vulkan it records the transfer, for an EXCLUSIVE buffer and
  // a CONCURRENT one; from this device's family, from none, or from another
  // family into a CONCURRENT buffer it records nothing. Either way the kernel
  // after it sees what an earlier batch wrote, and the layer stays silent.
  const std::vector<std::uint32_t> p = pattern(1);
  const std::uint32_t own = device().queue_family();
  const auto family_count =
      static_cast<std::uint32_t>(physical().queue_families().size());
  const auto taken_over = [&](const Buffer& buffer, std::uint32_t from) {
    upload(buffer, p);
    std::vector<std::uint32_t> back(kCount, 0);
    CommandBatch batch(device(), allocator());
    return batch.acquire(buffer, from).ok() && add_to(batch, buffer, 2).ok() &&
           batch.readback(buffer, 0, kBytes, back.data()).ok() &&
           batch.submit().ok() && back == plus(p, 2);
  };
  EXPECT_TRUE(taken_over(a_, VK_QUEUE_FAMILY_EXTERNAL));
  EXPECT_TRUE(taken_over(a_, VK_QUEUE_FAMILY_IGNORED));
  EXPECT_TRUE(taken_over(a_, own));
  if (family_count > 1) {
    const std::uint32_t other = own == 0 ? 1 : 0;
    const std::uint32_t both[2] = {own, other};
    Result<Buffer> shared =
        device_storage_buffer(allocator(), kBytes, 0, both, 2);
    ASSERT_TRUE(shared.ok());
    EXPECT_EQ(shared->sharing_mode(), VK_SHARING_MODE_CONCURRENT);
    EXPECT_TRUE(taken_over(*shared, VK_QUEUE_FAMILY_EXTERNAL));
    EXPECT_TRUE(taken_over(*shared, other));
  }

  // Refused, poisoning the batch: an empty buffer, and a family the device
  // does not have.
  EXPECT_TRUE(
      refused([&](CommandBatch& c) { return c.acquire(Buffer(), own); }));
  EXPECT_TRUE(
      refused([&](CommandBatch& c) { return c.acquire(a_, family_count); }));
}

TEST_F(BatchTest, ReleasesABufferAfterEverythingThatUsesIt) {
  // To outside Vulkan it records the transfer after every command, so a
  // kernel and a readback recorded after the release still run first, on the
  // buffer the batch owns; the next batch takes it back from there -- a frame
  // of CUDA's decoder loop -- for an EXCLUSIVE buffer and a CONCURRENT one.
  // To this device's family, to none, or a CONCURRENT buffer to another
  // family, it records nothing. The layer stays silent throughout.
  const std::vector<std::uint32_t> p = pattern(1);
  const std::uint32_t own = device().queue_family();
  const auto family_count =
      static_cast<std::uint32_t>(physical().queue_families().size());
  const auto handed_back = [&](const Buffer& buffer, std::uint32_t to) {
    upload(buffer, p);
    std::vector<std::uint32_t> back(kCount, 0);
    CommandBatch batch(device(), allocator());
    if (!(batch.release(buffer, to).ok() && add_to(batch, buffer, 2).ok() &&
          batch.readback(buffer, 0, kBytes, back.data()).ok() &&
          batch.submit().ok() && back == plus(p, 2))) {
      return false;
    }
    CommandBatch next(device(), allocator());
    return next.acquire(buffer, to).ok() && add_to(next, buffer, 3).ok() &&
           next.readback(buffer, 0, kBytes, back.data()).ok() &&
           next.submit().ok() && back == plus(p, 5);
  };
  EXPECT_TRUE(handed_back(a_, VK_QUEUE_FAMILY_EXTERNAL));
  EXPECT_TRUE(handed_back(a_, VK_QUEUE_FAMILY_IGNORED));
  EXPECT_TRUE(handed_back(a_, own));
  if (family_count > 1) {
    const std::uint32_t other = own == 0 ? 1 : 0;
    const std::uint32_t both[2] = {own, other};
    Result<Buffer> shared =
        device_storage_buffer(allocator(), kBytes, 0, both, 2);
    ASSERT_TRUE(shared.ok());
    EXPECT_TRUE(handed_back(*shared, VK_QUEUE_FAMILY_EXTERNAL));
    EXPECT_TRUE(handed_back(*shared, other));
  }

  // A batch that only hands a buffer back still submits it.
  {
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.release(a_, VK_QUEUE_FAMILY_EXTERNAL).ok());
    ASSERT_TRUE(batch.submit().ok());
    std::vector<std::uint32_t> back(kCount, 0);
    CommandBatch next(device(), allocator());
    ASSERT_TRUE(next.acquire(a_, VK_QUEUE_FAMILY_EXTERNAL).ok());
    ASSERT_TRUE(next.readback(a_, 0, kBytes, back.data()).ok());
    ASSERT_TRUE(next.submit().ok());
    EXPECT_EQ(back, plus(p, 5));
  }

  // Refused, poisoning the batch: an empty buffer, and a family the device
  // does not have.
  EXPECT_TRUE(
      refused([&](CommandBatch& c) { return c.release(Buffer(), own); }));
  EXPECT_TRUE(
      refused([&](CommandBatch& c) { return c.release(a_, family_count); }));
}

// --- image copies ------------------------------------------------------------

TEST_F(BatchTest, CopiesImagesIntoABuffer) {
  // One R8 and one R8G8, each a region short of the image, rising through the
  // buffer after a fill and before a kernel; then the first again back over
  // the second, which keeps its barrier and lands last.
  std::vector<std::uint8_t> luma(std::size_t{7} * 5);
  std::vector<std::uint8_t> chroma(std::size_t{4} * 3 * 2);
  for (std::size_t i = 0; i < luma.size(); ++i) {
    luma[i] = static_cast<std::uint8_t>((3 * i) + 1);
  }
  for (std::size_t i = 0; i < chroma.size(); ++i) {
    chroma[i] = static_cast<std::uint8_t>(200 - i);
  }
  const Image y = make_image(VK_FORMAT_R8_UNORM, 7, 5, luma);
  const Image c = make_image(VK_FORMAT_R8G8_UNORM, 4, 3, chroma);
  ASSERT_TRUE(y.valid() && c.valid());
  EXPECT_EQ(y.layout(), VK_IMAGE_LAYOUT_GENERAL);
  std::vector<std::uint8_t> want(kBytes, 1);  // the kernel adds 1 a byte
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t x = 0; x < 6; ++x) {
      want[(r * 6) + x] = static_cast<std::uint8_t>(luma[(r * 7) + x] + 1);
    }
  }
  for (std::size_t r = 0; r < 2; ++r) {
    for (std::size_t x = 0; x < 6; ++x) {
      want[24 + (r * 6) + x] =
          static_cast<std::uint8_t>(chroma[(r * 8) + x] + 1);
    }
  }
  want[24] = static_cast<std::uint8_t>(luma[0] + 1);
  want[25] = static_cast<std::uint8_t>(luma[1] + 1);
  std::vector<std::uint8_t> got(kBytes, 0);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.fill(a_, 0, kBytes, 0U).ok());
  ASSERT_TRUE(batch.copy(y, 6, 4, a_, 0).ok());
  ASSERT_TRUE(batch.copy(c, 3, 2, a_, 24).ok());
  ASSERT_TRUE(batch.copy(y, 2, 1, a_, 24).ok());
  ASSERT_TRUE(add_to(batch, a_, 0x01010101U).ok());
  ASSERT_TRUE(batch.readback(a_, 0, kBytes, got.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(got, want);
}

TEST_F(BatchTest, CopiesWiderTexels) {
  // RGBA8 and R32F, formats recon's copy refused.
  std::vector<std::uint8_t> rgba(std::size_t{3} * 2 * 4);
  for (std::size_t i = 0; i < rgba.size(); ++i) {
    rgba[i] = static_cast<std::uint8_t>((i * 5) + 2);
  }
  const Image color = make_image(VK_FORMAT_R8G8B8A8_UNORM, 3, 2, rgba);
  const std::vector<float> depth_values = {0.5F, 1.25F, 2.0F, 4.5F};
  std::vector<std::uint8_t> depth(depth_values.size() * 4);
  std::memcpy(depth.data(), depth_values.data(), depth.size());
  const Image depth_image = make_image(VK_FORMAT_R32_SFLOAT, 2, 2, depth);
  ASSERT_TRUE(color.valid() && depth_image.valid());
  std::vector<std::uint8_t> got_color(rgba.size(), 0);
  std::vector<float> got_depth(depth_values.size(), 0.0F);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.copy(color, 3, 2, a_, 0).ok());
  ASSERT_TRUE(batch.copy(depth_image, 2, 2, a_, 64).ok());
  ASSERT_TRUE(batch.readback(a_, 0, rgba.size(), got_color.data()).ok());
  ASSERT_TRUE(batch.readback(a_, 64, 16, got_depth.data()).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(got_color, rgba);
  EXPECT_EQ(got_depth, depth_values);
}

TEST_F(BatchTest, RefusesImageCopiesItCannotMake) {
  const Image y =
      make_image(VK_FORMAT_R8_UNORM, 7, 5, std::vector<std::uint8_t>(35, 0));
  const Image unusable = make_image(VK_FORMAT_R8_UNORM, 1, 1, {0}, /*usage=*/0);
  const Image fresh = [&] {
    ImageDesc desc;
    desc.extent = {4, 4};
    desc.format = VK_FORMAT_R8_UNORM;
    desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.with_view = false;
    Result<Image> made = allocator().create_image(desc);
    return made.ok() ? *std::move(made) : Image{};
  }();
  ASSERT_TRUE(y.valid() && unusable.valid() && fresh.valid());
  // Before anything reaches Vulkan, so no image need exist.
  EXPECT_TRUE(refused([&](CommandBatch& b) {
    return b.copy(fake(VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED), 1,
                  1, a_, 0);
  }));
  EXPECT_TRUE(refused([&](CommandBatch& b) {
    return b.copy(fake(VK_FORMAT_R8_UNORM, 1, 1,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                  1, 1, a_, 0);
  }));
  EXPECT_TRUE(refused([&](CommandBatch& b) {
    return b.copy(fake(VK_FORMAT_D32_SFLOAT, 1, 1, VK_IMAGE_LAYOUT_GENERAL), 1,
                  1, a_, 0);
  }));
  EXPECT_TRUE(refused([&](CommandBatch& b) {
    return b.copy(fake(VK_FORMAT_R8_UNORM, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
                       VK_SAMPLE_COUNT_4_BIT),
                  1, 1, a_, 0);
  }));
  // 2^63 + 2 texels of two bytes, which wrap to 4 bytes.
  EXPECT_TRUE(refused([&](CommandBatch& b) {
    const std::uint32_t w = 0xFFFE0002U;
    const std::uint32_t h = 0x80010001U;
    return b.copy(fake(VK_FORMAT_R8G8_UNORM, w, h, VK_IMAGE_LAYOUT_GENERAL), w,
                  h, a_, 0);
  }));
  // An R32 texel at an offset of 4 bytes is fine; at 2 it is not.
  EXPECT_TRUE(refused([&](CommandBatch& b) {
    return b.copy(
        fake(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, VK_IMAGE_LAYOUT_GENERAL), 1,
        1, a_, 4);
  }));
  EXPECT_TRUE(
      refused([&](CommandBatch& b) { return b.copy(Image(), 1, 1, a_, 0); }));
  EXPECT_TRUE(refused([&](CommandBatch& b) { return b.copy(y, 8, 5, a_, 0); }));
  EXPECT_TRUE(refused([&](CommandBatch& b) { return b.copy(y, 7, 0, a_, 0); }));
  EXPECT_TRUE(refused([&](CommandBatch& b) { return b.copy(y, 7, 5, a_, 2); }));
  EXPECT_TRUE(refused(
      [&](CommandBatch& b) { return b.copy(y, 7, 5, a_, kBytes - 32); }));
  EXPECT_TRUE(
      refused([&](CommandBatch& b) { return b.copy(unusable, 1, 1, a_, 0); }));
  // An allocated image whose layout nobody recorded reads as UNDEFINED.
  EXPECT_TRUE(
      refused([&](CommandBatch& b) { return b.copy(fresh, 1, 1, a_, 0); }));
}

// --- refusals ----------------------------------------------------------------

TEST_F(BatchTest, ARefusalPoisonsTheBatch) {
  const std::vector<std::uint32_t> z(kCount, 0);
  const std::vector<std::uint32_t> p = pattern(1);
  upload(a_, z);
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.upload(a_, 0, p.data(), kBytes).ok());
  EXPECT_TRUE(is_invalid(batch.upload(a_, 4, p.data(), kBytes)));  // past end
  EXPECT_TRUE(is_invalid(batch.fill(a_, 0, 4, 0)));                // poisoned
  EXPECT_TRUE(is_invalid(batch.submit()));
  EXPECT_TRUE(batch.submitted());
  EXPECT_TRUE(is_invalid(batch.submit()));
  EXPECT_EQ(read(a_), z);  // the first upload never ran
}

TEST_F(BatchTest, RefusesBadArguments) {
  BufferDesc desc;
  desc.size = kBytes;
  desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  desc.memory = MemoryUsage::DeviceOnly;
  Result<Buffer> bare_made = allocator().create_buffer(desc);
  ASSERT_TRUE(bare_made.ok());
  const Buffer bare = *std::move(bare_made);
  // Device-mapped without the transfer bits: refused all the same, as a batch
  // writes and reads nothing through a mapping. Where the device has no
  // device-mapped memory, an empty buffer, refused too.
  Result<Buffer> mapped_made = mapped_storage_buffer(allocator(), kBytes);
  ASSERT_EQ(mapped_made.ok(), physical().device_mapped_memory());
  const Buffer mapped_bare =
      mapped_made.ok() ? *std::move(mapped_made) : Buffer{};
  const std::vector<std::uint32_t> p = pattern(1);
  std::vector<std::uint32_t> got(kCount, 0);
  const auto refuses = [&](auto&& call) {
    CommandBatch batch(device(), allocator());
    return is_invalid(call(batch));
  };
  // Usage.
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.upload(bare, 0, p.data(), 4); }));
  EXPECT_TRUE(refuses(
      [&](CommandBatch& c) { return c.reserve_upload(bare, 0, 4).status(); }));
  EXPECT_TRUE(refuses(
      [&](CommandBatch& c) { return c.upload(mapped_bare, 0, p.data(), 4); }));
  EXPECT_TRUE(refuses(
      [&](CommandBatch& c) { return c.readback(bare, 0, 4, got.data()); }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.readback(mapped_bare, 0, 4, got.data());
  }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) { return c.fill(bare, 0, 4, 0); }));
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.copy(a_, 0, bare, 0, 4); }));
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.copy(bare, 0, a_, 0, 4); }));
  // Ranges, alignment, overlap, nulls.
  EXPECT_TRUE(refuses(
      [&](CommandBatch& c) { return c.upload(a_, kBytes, p.data(), 4); }));
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.upload(a_, 0, nullptr, 4); }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.reserve_upload(a_, kBytes - 2, 4).status();
  }));
  EXPECT_TRUE(refuses(
      [&](CommandBatch& c) { return c.reserve_upload(a_, 0, 0).status(); }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) { return c.fill(a_, 2, 4, 0); }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) { return c.fill(a_, 0, 6, 0); }));
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.zero(a_, kBytes - 2, 4); }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) { return c.zero(bare, 0, 4); }));
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.copy(a_, 0, a_, 8, 16); }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.readback(a_, kBytes - 2, 4, got.data());
  }));
  EXPECT_TRUE(
      refuses([&](CommandBatch& c) { return c.readback(a_, 0, 4, nullptr); }));
  if (max_groups_ < 0xFFFFFFFFU) {
    EXPECT_TRUE(refuses([&](CommandBatch& c) {
      const Push push{kCount, 0};
      return c.dispatch(add_, &push, sizeof(push), max_groups_ + 1,
                        max_groups_);
    }));
  }
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.dispatch(add_, nullptr, sizeof(Push), 1, max_groups_);
  }));
  // A push off 4 bytes, or past the kernel's range; an unbuilt kernel.
  const std::uint32_t words[3] = {kCount, 0, 0};
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.dispatch(add_, words, 6, 1, max_groups_);
  }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.dispatch(add_, words, sizeof(words), 1, max_groups_);
  }));
  EXPECT_TRUE(refuses([&](CommandBatch& c) {
    return c.dispatch(ComputeKernel{}, nullptr, 0, 1, max_groups_);
  }));
  // Nothing at all is fine, and submits nothing.
  CommandBatch empty(device(), allocator());
  EXPECT_TRUE(empty.upload(a_, 0, nullptr, 0).ok());
  EXPECT_TRUE(empty.readback(a_, 0, 0, nullptr).ok());
  EXPECT_TRUE(empty.submit().ok());
}

TEST_F(BatchTest, ABatchWithNoAllocatorStagesNothing) {
  // It dispatches and uploads inline, and refuses what would need staging.
  const std::vector<std::uint32_t> p = pattern(1);
  std::uint32_t word = 0;
  CommandBatch inline_only(device());
  EXPECT_TRUE(inline_only.upload(a_, 0, p.data(), kBytes).ok());
  EXPECT_TRUE(is_invalid(inline_only.upload(a_, 2, p.data(), 4)));
  CommandBatch reserves(device());
  EXPECT_TRUE(is_invalid(reserves.reserve_upload(a_, 0, 4).status()));
  CommandBatch reads(device());
  EXPECT_TRUE(is_invalid(reads.readback(a_, 0, 4, &word)));
}

TEST_F(BatchTest, AMovedFromDeviceOrAllocatorPoisonsTheBatch) {
  Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  Device other = *std::move(made);
  const Device taken(std::move(other));
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  CommandBatch batch(other, allocator());
  EXPECT_TRUE(is_invalid(batch.fill(a_, 0, 4, 0)));
  EXPECT_TRUE(is_invalid(batch.submit()));

  Allocator moved_allocator = std::move(allocator());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  CommandBatch no_allocator(device(), allocator());
  EXPECT_TRUE(is_invalid(no_allocator.fill(a_, 0, 4, 0)));
  allocator() = std::move(moved_allocator);
}

TEST_F(BatchTest, MovesCarryTheRecordedCommands) {
  std::uint32_t word = 0;
  CommandBatch source(device(), allocator());
  ASSERT_TRUE(source.fill(a_, 0, 4, 41U).ok());
  ASSERT_TRUE(source.readback(a_, 0, 4, &word).ok());
  CommandBatch moved(std::move(source));
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(source.submitted());
  EXPECT_TRUE(is_invalid(source.fill(a_, 0, 4, 0)));
  EXPECT_TRUE(is_invalid(source.submit()));
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_TRUE(moved.submit().ok());
  EXPECT_EQ(word, 41U);

  // Over a live batch, whose own commands are dropped.
  CommandBatch next(device(), allocator());
  ASSERT_TRUE(next.fill(a_, 0, 4, 42U).ok());
  ASSERT_TRUE(next.readback(a_, 0, 4, &word).ok());
  CommandBatch live(device(), allocator());
  ASSERT_TRUE(live.fill(a_, 0, 4, 7U).ok());
  live = std::move(next);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(is_invalid(next.submit()));
  ASSERT_TRUE(live.submit().ok());
  EXPECT_EQ(word, 42U);

  // A self-move keeps the batch.
  CommandBatch self(device(), allocator());
  ASSERT_TRUE(self.fill(a_, 0, 4, 43U).ok());
  ASSERT_TRUE(self.readback(a_, 0, 4, &word).ok());
  CommandBatch* alias = &self;
  self = std::move(*alias);
  ASSERT_TRUE(self.submit().ok());
  EXPECT_EQ(word, 43U);
}

#ifdef VKC_TEST_EXCEPTIONS
TEST_F(BatchTest, ARecordThatThrowsGivesItsBufferBackReset) {
  // The next submit begins the same buffer, which the layer refuses while it
  // is still recording.
  bool threw = false;
  try {
    static_cast<void>(device().submit_single_time(
        [](VkCommandBuffer) { throw std::runtime_error("record"); }));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  EXPECT_TRUE(device().submit_single_time([](VkCommandBuffer) {}).ok());
}
#endif

// --- threads -----------------------------------------------------------------

// Several threads batch on one device at once, each with its own kernel and
// buffer: an inline upload, one staged past the inline limit, a dispatch and
// two readbacks a round. On the created device, and on one adopted with no
// embedder mutex, whose queue the device must lock itself. The layer reports
// a race as an error.
TEST_F(BatchTest, BatchesOnSeveralThreadsShareADevice) {
  constexpr int kThreads = 4;
  constexpr int kRounds = 25;
  constexpr VkDeviceSize kStaged = CommandBatch::kMaxInlineUpload * 2;
  std::vector<ComputeKernel> kernels(kThreads);
  KernelSetBuilder per_thread(device());
  for (ComputeKernel& k : kernels) {
    ASSERT_TRUE(per_thread
                    .add(k, "test_add", vkc_test_add_comp_spv,
                         vkc_test_add_comp_spv_size, 1, &range_)
                    .ok());
  }
  const Result<DescriptorPool> pool = per_thread.build();
  ASSERT_TRUE(pool.ok());

  const auto run_threads = [&](const Device& on) {
    std::atomic<int> failed{0};
    std::mutex why_mutex;
    std::string why;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        const auto fail = [&](const std::string& reason) {
          ++failed;
          const std::scoped_lock lock(why_mutex);
          why = reason;
        };
        Result<Buffer> made =
            device_storage_buffer(allocator(), kBytes + kStaged);
        if (!made.ok()) {
          fail(made.status().message());
          return;
        }
        const Buffer& mine = *made;
        const std::vector<std::uint32_t> staged =
            pattern(7U * static_cast<std::uint32_t>(t), kStaged / 4);
        for (int r = 0; r < kRounds; ++r) {
          const std::vector<std::uint32_t> in =
              pattern((1000U * static_cast<std::uint32_t>(t)) +
                      static_cast<std::uint32_t>(r));
          std::vector<std::uint32_t> out(kCount, 0);
          std::uint32_t last = 0;
          // A failed call poisons the batch, so submit returns the first
          // refusal: each recording call's own Status is deliberately
          // discarded, and submit's is checked.
          CommandBatch batch(on, allocator());
          (void)batch.upload(mine, 0, in.data(), kBytes);
          const VkDeviceSize after_inline = kBytes;
          (void)batch.upload(mine, after_inline, staged.data(), kStaged);
          kernels[t].set.write_storage_buffer(0, mine.handle(), 0,
                                              VK_WHOLE_SIZE);
          const Push push{kCount, 3};
          (void)batch.dispatch(kernels[t], &push, sizeof(push),
                               group_count(kCount, 64), max_groups_);
          (void)batch.readback(mine, 0, kBytes, out.data());
          (void)batch.readback(mine, kBytes + kStaged - 4, 4, &last);
          const Status submitted = batch.submit();
          if (!submitted.ok()) {
            fail(submitted.message());
          } else if (out != plus(in, 3) || last != staged.back()) {
            fail("wrong result in round " + std::to_string(r));
          }
        }
      });
    }
    for (std::thread& t : threads) t.join();
    EXPECT_EQ(failed.load(), 0) << why;
  };
  run_threads(device());

  AdoptedDevice adopted;
  adopted.instance = instance().handle();
  adopted.instance_api_version = instance().api_version();
  adopted.physical_device = device().physical_device();
  adopted.device = device().handle();
  adopted.queue_family = device().queue_family();
  adopted.queue = device().queue();
  adopted.enabled_timeline_semaphore = true;
  Result<Device> borrowed = Device::adopt(adopted, {});
  ASSERT_TRUE(borrowed.ok()) << borrowed.status().message();
  run_threads(*borrowed);
}

}  // namespace
}  // namespace volumetric_kit::core
