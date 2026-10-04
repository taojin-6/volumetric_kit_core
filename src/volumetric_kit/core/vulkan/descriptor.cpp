// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/descriptor.hpp"

#include <cstdint>
#include <memory>
#include <string>

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
  pool.lifetime_ = std::make_shared<char>();
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
  DescriptorSet out(pool_.device(), set);
  out.state_->pool = lifetime_;
  out.state_->pooled = true;
  return out;
}

DescriptorSet::DescriptorSet(VkDevice device, VkDescriptorSet set)
    : state_(std::make_shared<State>()) {
  state_->device = device;
  state_->set = set;
}

void DescriptorSet::write(const char* caller, VkDescriptorType type,
                          std::uint32_t binding,
                          const VkDescriptorBufferInfo* buffer,
                          const VkDescriptorImageInfo* image) const {
  VKC_CHECK(valid(), std::string(caller) + " on an empty set");
  // A null resource is undefined without nullDescriptor, and with layers off
  // surfaces, if at all, as a fault at a dispatch far from here.
  if (buffer != nullptr) {
    VKC_CHECK(buffer->buffer != VK_NULL_HANDLE,
              std::string(caller) + ": the buffer is null");
  } else {
    VKC_CHECK(image->imageView != VK_NULL_HANDLE,
              std::string(caller) + ": the image view is null");
  }
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = state_->set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = type;
  w.pBufferInfo = buffer;
  w.pImageInfo = image;
  vkUpdateDescriptorSets(state_->device, 1, &w, 0, nullptr);
  ++state_->writes;
}

void DescriptorSet::write_storage_buffer(std::uint32_t binding, VkBuffer buffer,
                                         VkDeviceSize offset,
                                         VkDeviceSize range) const {
  const VkDescriptorBufferInfo info{buffer, offset, range};
  write("DescriptorSet::write_storage_buffer",
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding, &info, nullptr);
}

void DescriptorSet::write_uniform_buffer(std::uint32_t binding, VkBuffer buffer,
                                         VkDeviceSize offset,
                                         VkDeviceSize range) const {
  const VkDescriptorBufferInfo info{buffer, offset, range};
  write("DescriptorSet::write_uniform_buffer",
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, binding, &info, nullptr);
}

void DescriptorSet::write_combined_image_sampler(std::uint32_t binding,
                                                 VkImageView view,
                                                 VkSampler sampler,
                                                 VkImageLayout layout) const {
  const VkDescriptorImageInfo info{sampler, view, layout};
  write("DescriptorSet::write_combined_image_sampler",
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, binding, nullptr, &info);
}

void DescriptorSet::write_storage_image(std::uint32_t binding, VkImageView view,
                                        VkImageLayout layout) const {
  const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
  write("DescriptorSet::write_storage_image", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        binding, nullptr, &info);
}

}  // namespace volumetric_kit::core
