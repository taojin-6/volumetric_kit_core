// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/descriptor.hpp"

#include <cstdint>
#include <memory>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<DescriptorSetLayout> DescriptorSetLayout::create(
    VkDevice device, const VkDescriptorSetLayoutBinding* bindings,
    std::uint32_t count) {
  // Arguments before the device, so a device-less test still reaches them.
  if (count > 0 && bindings == nullptr) {
    return Status::invalid_argument(
        "DescriptorSetLayout::create: null bindings with a non-zero count");
  }
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "DescriptorSetLayout::create: device is null");
  }
  VkDescriptorSetLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = count;
  info.pBindings = bindings;
  VkDescriptorSetLayout handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateDescriptorSetLayout(device, &info, nullptr, &handle));
  DescriptorSetLayout layout;
  layout.layout_ =
      UniqueHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>(device,
                                                                        handle);
  return layout;
}

Result<DescriptorPool> DescriptorPool::create(VkDevice device,
                                              const VkDescriptorPoolSize* sizes,
                                              std::uint32_t size_count,
                                              std::uint32_t max_sets) {
  if (sizes == nullptr || size_count == 0) {
    return Status::invalid_argument(
        "DescriptorPool::create: sizes must be non-null and non-empty");
  }
  if (max_sets == 0) {
    return Status::invalid_argument("DescriptorPool::create: max_sets is zero");
  }
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("DescriptorPool::create: device is null");
  }
  VkDescriptorPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.maxSets = max_sets;
  info.poolSizeCount = size_count;
  info.pPoolSizes = sizes;
  VkDescriptorPool handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateDescriptorPool(device, &info, nullptr, &handle));
  DescriptorPool pool;
  pool.pool_ =
      UniqueHandle<VkDescriptorPool, vkDestroyDescriptorPool>(device, handle);
  return pool;
}

Result<DescriptorSet> DescriptorPool::allocate(VkDescriptorSetLayout layout) {
  if (!pool_.valid()) {
    return Status::invalid_argument("DescriptorPool::allocate: pool is empty");
  }
  if (layout == VK_NULL_HANDLE) {
    return Status::invalid_argument("DescriptorPool::allocate: layout is null");
  }
  VkDescriptorSetAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  info.descriptorPool = pool_.get();
  info.descriptorSetCount = 1;
  info.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VKC_VK_TRY(vkAllocateDescriptorSets(pool_.device(), &info, &set));
  return DescriptorSet(pool_.device(), set);
}

DescriptorSet::DescriptorSet(VkDevice device, VkDescriptorSet set)
    : state_(std::make_shared<State>(State{device, set, 0})) {}

void DescriptorSet::write(const VkWriteDescriptorSet& write) const {
  vkUpdateDescriptorSets(state_->device, 1, &write, 0, nullptr);
  ++state_->writes;
}

void DescriptorSet::write_storage_buffer(std::uint32_t binding, VkBuffer buffer,
                                         VkDeviceSize offset,
                                         VkDeviceSize range) const {
  VKC_CHECK(valid(), "DescriptorSet::write_storage_buffer on an empty set");
  const VkDescriptorBufferInfo info{buffer, offset, range};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = state_->set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &info;
  write(w);
}

void DescriptorSet::write_uniform_buffer(std::uint32_t binding, VkBuffer buffer,
                                         VkDeviceSize offset,
                                         VkDeviceSize range) const {
  VKC_CHECK(valid(), "DescriptorSet::write_uniform_buffer on an empty set");
  const VkDescriptorBufferInfo info{buffer, offset, range};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = state_->set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w.pBufferInfo = &info;
  write(w);
}

void DescriptorSet::write_combined_image_sampler(std::uint32_t binding,
                                                 VkImageView view,
                                                 VkSampler sampler,
                                                 VkImageLayout layout) const {
  VKC_CHECK(valid(),
            "DescriptorSet::write_combined_image_sampler on an empty set");
  const VkDescriptorImageInfo info{sampler, view, layout};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = state_->set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &info;
  write(w);
}

void DescriptorSet::write_storage_image(std::uint32_t binding, VkImageView view,
                                        VkImageLayout layout) const {
  VKC_CHECK(valid(), "DescriptorSet::write_storage_image on an empty set");
  const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = state_->set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w.pImageInfo = &info;
  write(w);
}

}  // namespace volumetric_kit::core
