// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file compute_pipeline.hpp
/// @brief A compute `VkPipeline` and the `VkPipelineLayout` it is built on.

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class ShaderModule;

/// @brief Parameters for @ref ComputePipeline::create.
///
/// The pipeline layout is built from the descriptor-set layouts and
/// push-constant ranges given here, not reflected from the SPIR-V, so the
/// core takes no reflection dependency. They must match the shader's
/// `layout(set =, binding =)` and `push_constant` declarations: a mismatch is
/// undefined at dispatch, which the validation layer reports, not a create
/// error.
///
/// @code
/// // layout(set = 0, ...) buffers, and a push_constant block of Params.
/// const VkDescriptorSetLayout layouts[] = {layout.handle()};
/// const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
///                                sizeof(Params)};
/// ComputePipelineDesc desc;
/// desc.shader = &shader;
/// desc.set_layouts = layouts;
/// desc.set_layout_count = 1;
/// desc.push_ranges = &push;
/// desc.push_range_count = 1;
/// @endcode
struct ComputePipelineDesc {
  /// The compute stage; non-null and valid.
  const ShaderModule* shader = nullptr;
  /// Descriptor-set layouts, one per set index; null only when the count is
  /// 0.
  const VkDescriptorSetLayout* set_layouts = nullptr;
  /// The length of @ref set_layouts.
  std::uint32_t set_layout_count = 0;
  /// Push-constant ranges; null only when the count is 0.
  const VkPushConstantRange* push_ranges = nullptr;
  /// The length of @ref push_ranges.
  std::uint32_t push_range_count = 0;
  /// The shader's entry point; non-null.
  const char* entry_point = "main";
};

/// @brief Owns a compute `VkPipeline` and its `VkPipelineLayout`.
///
/// @warning The device passed to @ref create must outlive the pipeline.
///
/// @code
/// const VkDescriptorSetLayout layouts[] = {layout.handle()};
/// ComputePipelineDesc desc;
/// desc.shader = &shader;
/// desc.set_layouts = layouts;
/// desc.set_layout_count = 1;
/// VKC_ASSIGN(ComputePipeline pipeline,
///            ComputePipeline::create(device.handle(), desc));
/// vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
/// @endcode
class VKC_VULKAN_API ComputePipeline {
 public:
  /// @brief Create a compute pipeline per @p desc.
  /// @param device  The device to create it on.
  /// @param desc    The compute stage, descriptor-set layouts, and push ranges.
  /// @return The pipeline; @ref Status::Code::InvalidArgument for a null or
  ///         invalid shader, a null entry point, a null array with a count, or
  ///         a null @p device; or a backend @ref Status.
  static Result<ComputePipeline> create(VkDevice device,
                                        const ComputePipelineDesc& desc);

  /// @brief Construct an empty pipeline; @ref valid is false.
  ComputePipeline() noexcept = default;
  ComputePipeline(ComputePipeline&&) noexcept = default;
  ComputePipeline& operator=(ComputePipeline&&) noexcept = default;
  ComputePipeline(const ComputePipeline&) = delete;
  ComputePipeline& operator=(const ComputePipeline&) = delete;
  ~ComputePipeline() = default;

  /// @return The pipeline (`VK_NULL_HANDLE` when empty).
  VkPipeline handle() const noexcept { return pipeline_.get(); }
  /// @return The pipeline layout, which binds descriptor sets and pushes.
  VkPipelineLayout layout() const noexcept { return layout_.get(); }
  /// @return Whether this owns a pipeline.
  bool valid() const noexcept { return pipeline_.valid(); }

 private:
  // Layout before pipeline, so destruction (reverse order) frees the pipeline
  // first, then the layout it was built on.
  UniqueHandle<VkPipelineLayout, vkDestroyPipelineLayout> layout_;
  UniqueHandle<VkPipeline, vkDestroyPipeline> pipeline_;
};

}  // namespace volumetric_kit::core
