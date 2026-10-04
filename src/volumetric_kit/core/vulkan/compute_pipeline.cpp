// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/compute_pipeline.hpp"

#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/shader.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<ComputePipeline> ComputePipeline::create(
    VkDevice device, const ComputePipelineDesc& desc) {
  if (desc.shader == nullptr || !desc.shader->valid()) {
    return Status::invalid_argument(
        "ComputePipeline::create: shader is null or empty");
  }
  if (desc.entry_point == nullptr) {
    return Status::invalid_argument(
        "ComputePipeline::create: entry_point is null");
  }
  if (desc.set_layout_count > 0 && desc.set_layouts == nullptr) {
    return Status::invalid_argument(
        "ComputePipeline::create: null set_layouts with a non-zero count");
  }
  if (desc.push_range_count > 0 && desc.push_ranges == nullptr) {
    return Status::invalid_argument(
        "ComputePipeline::create: null push_ranges with a non-zero count");
  }
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("ComputePipeline::create: device is null");
  }

  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = desc.set_layout_count;
  layout_info.pSetLayouts = desc.set_layouts;
  layout_info.pushConstantRangeCount = desc.push_range_count;
  layout_info.pPushConstantRanges = desc.push_ranges;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));
  // Owned at once, so a failed pipeline create below still frees it.
  UniqueHandle<VkPipelineLayout, vkDestroyPipelineLayout> owned_layout(device,
                                                                       layout);

  // Zeroed, then set: `stage` has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = desc.shader->handle();
  stage.pName = desc.entry_point;

  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage = stage;
  pipeline_info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                      nullptr, &pipeline));

  ComputePipeline result;
  result.layout_ = std::move(owned_layout);
  result.pipeline_ =
      UniqueHandle<VkPipeline, vkDestroyPipeline>(device, pipeline);
  return result;
}

}  // namespace volumetric_kit::core
