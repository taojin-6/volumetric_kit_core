// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/compute_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_pipeline.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/shader.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

// A build failure, prefixed with the kernel it belongs to: one create()
// registers several kernels, and the failure names only the Vulkan call.
Status named(const char* name, Status why) {
  return std::move(why).with_context(name != nullptr ? name
                                                     : "<unnamed kernel>");
}

// VKC_ASSIGN, but naming the kernel on failure, so no build failure reaches a
// caller unnamed.
template <typename T>
Status assign_named(T& out, Result<T>&& from, const char* name) {
  if (!from) return named(name, std::move(from).status());
  out = *std::move(from);
  return {};
}

}  // namespace

Status KernelSetBuilder::add(ComputeKernel& out, const char* name,
                             const unsigned char* spv, std::size_t spv_size,
                             std::uint32_t bindings,
                             const VkPushConstantRange* push) {
  if (push != nullptr && push->offset != 0) {
    return named(name, Status::invalid_argument(
                           "the push range must start at offset 0"));
  }
  // Built aside and moved in whole, so a failure leaves `out` as it was, and
  // a kernel built again holds no set of its old layout until build().
  ComputeKernel kernel;
  kernel.name = name;
  kernel.push_bytes = push != nullptr ? push->size : 0;
  kernel.bindings = bindings;

  // Set 0: `bindings` compute-stage storage buffers, matched by index.
  std::vector<VkDescriptorSetLayoutBinding> b(bindings);
  for (std::uint32_t i = 0; i < bindings; ++i) {
    b[i].binding = i;
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VKC_TRY(assign_named(
      kernel.layout,
      DescriptorSetLayout::create(device_->handle(), b.data(), bindings),
      name));

  // The module is transient: the pipeline does not retain it. SPIR-V is a
  // stream of words, and vkc_embed_shaders aligns its arrays to 4.
  ShaderModule module;
  VKC_TRY(assign_named(
      module,
      ShaderModule::create(device_->handle(),
                           // NOLINTNEXTLINE(*-reinterpret-cast)
                           reinterpret_cast<const std::uint32_t*>(spv),
                           spv_size),
      name));
  VkDescriptorSetLayout layout = kernel.layout.handle();
  ComputePipelineDesc desc;
  desc.shader = &module;
  desc.set_layouts = &layout;
  desc.set_layout_count = 1;
  desc.push_ranges = push;
  desc.push_range_count = push != nullptr ? 1U : 0U;
  VKC_TRY(assign_named(kernel.pipeline,
                       ComputePipeline::create(device_->handle(), desc), name));

  // Name what a capture indexes by, not only the dispatch's region: Nsight
  // groups by VkPipeline, and MoltenVK labels the MTLComputePipelineState
  // with a named pipeline's name.
  device_->set_object_name(VK_OBJECT_TYPE_PIPELINE,
                           debug_object_handle(kernel.pipeline.handle()), name);
  device_->set_object_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                           debug_object_handle(kernel.pipeline.layout()), name);
  device_->set_object_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                           debug_object_handle(kernel.layout.handle()), name);

  out = std::move(kernel);
  kernels_.push_back(&out);
  descriptor_total_ += bindings;
  return {};
}

Result<DescriptorPool> KernelSetBuilder::build() {
  // A pool needs a non-zero descriptor count, so a group of binding-less
  // kernels still sizes one.
  VkDescriptorPoolSize size{};
  size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  size.descriptorCount = descriptor_total_ > 0 ? descriptor_total_ : 1;
  const auto max_sets = static_cast<std::uint32_t>(kernels_.size());
  VKC_ASSIGN(DescriptorPool pool,
             DescriptorPool::create(device_->handle(), &size, 1,
                                    max_sets > 0 ? max_sets : 1));
  // Every set before any is handed out: a failure part-way destroys the local
  // pool, and the sets made from it, with the kernels untouched.
  std::vector<DescriptorSet> sets;
  sets.reserve(kernels_.size());
  for (const ComputeKernel* kernel : kernels_) {
    VKC_ASSIGN(DescriptorSet set, pool.allocate(kernel->layout.handle()));
    sets.push_back(std::move(set));
  }
  for (std::size_t i = 0; i < kernels_.size(); ++i) {
    kernels_[i]->set = sets[i];
  }
  return pool;
}

Status KernelSets::reserve(const Device& device, const ComputeKernel& kernel,
                           std::uint32_t count) {
  if (!kernel.valid() || count == 0) {
    return Status::invalid_argument(
        "KernelSets::reserve: needs a built kernel and a count above 0");
  }
  if (count <= sets_.size()) return {};
  VkDescriptorPoolSize size{};
  size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  size.descriptorCount = kernel.bindings > 0 ? kernel.bindings * count : 1;
  VKC_ASSIGN(DescriptorPool pool,
             DescriptorPool::create(device.handle(), &size, 1, count));
  std::vector<DescriptorSet> sets;
  sets.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    VKC_ASSIGN(DescriptorSet set, pool.allocate(kernel.layout.handle()));
    sets.push_back(std::move(set));
  }
  pool_ = std::move(pool);
  sets_ = std::move(sets);
  return {};
}

Status dispatch(const Device& device, const ComputeKernel& kernel,
                const void* push, std::uint32_t push_size, std::uint32_t groups,
                std::uint32_t max_groups, GpuStageScope* stage) {
  // A batch of one, so the checks, the span, the label and the barrier have
  // one definition.
  CommandBatch batch(device);
  VKC_TRY(batch.dispatch(kernel, push, push_size, groups, max_groups, stage));
  return batch.submit();
}

}  // namespace volumetric_kit::core
