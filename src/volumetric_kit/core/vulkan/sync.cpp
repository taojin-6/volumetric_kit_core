// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/sync.hpp"

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<Fence> Fence::create(VkDevice device, bool signaled) {
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("Fence::create: device is null");
  }
  VkFenceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (signaled) info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkFence handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateFence(device, &info, nullptr, &handle));
  Fence fence;
  fence.handle_ = UniqueHandle<VkFence, vkDestroyFence>(device, handle);
  return fence;
}

Status Fence::wait(std::uint64_t timeout_ns) const {
  if (!valid())
    return Status::invalid_argument("Fence::wait on an empty fence");
  VkFence fence = handle_.get();
  const VkResult result =
      vkWaitForFences(handle_.device(), 1, &fence, VK_TRUE, timeout_ns);
  // VK_TIMEOUT too: reported, for the caller to tell apart with vk_result().
  if (result != VK_SUCCESS) return vk_error(result, "vkWaitForFences");
  return {};
}

Status Fence::reset() {
  if (!valid()) {
    return Status::invalid_argument("Fence::reset on an empty fence");
  }
  VkFence fence = handle_.get();
  VKC_VK_TRY(vkResetFences(handle_.device(), 1, &fence));
  return {};
}

bool Fence::is_signaled() const {
  return valid() &&
         vkGetFenceStatus(handle_.device(), handle_.get()) == VK_SUCCESS;
}

Result<Semaphore> Semaphore::create(VkDevice device) {
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("Semaphore::create: device is null");
  }
  VkSemaphoreCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkSemaphore handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateSemaphore(device, &info, nullptr, &handle));
  Semaphore semaphore;
  semaphore.handle_ =
      UniqueHandle<VkSemaphore, vkDestroySemaphore>(device, handle);
  return semaphore;
}

Result<TimelineSemaphore> TimelineSemaphore::create(
    const Device& device, std::uint64_t initial_value) {
  // The feature alone: any queue, and the version the feature itself needs.
  DeviceRequirements timeline;
  timeline.api_version = VK_API_VERSION_1_0;
  timeline.queue_flags = 0;
  timeline.timeline_semaphore = true;
  VKC_TRY(
      device.check_enabled(timeline).with_context("TimelineSemaphore::create"));
  VkSemaphoreTypeCreateInfo type{};
  type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type.initialValue = initial_value;
  VkSemaphoreCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  info.pNext = &type;
  VkSemaphore handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateSemaphore(device.handle(), &info, nullptr, &handle));
  TimelineSemaphore semaphore;
  semaphore.handle_ =
      UniqueHandle<VkSemaphore, vkDestroySemaphore>(device.handle(), handle);
  return semaphore;
}

Result<std::uint64_t> TimelineSemaphore::value() const {
  if (!valid()) {
    return Status::invalid_argument(
        "TimelineSemaphore::value on an empty semaphore");
  }
  std::uint64_t value = 0;
  VKC_VK_TRY(
      vkGetSemaphoreCounterValue(handle_.device(), handle_.get(), &value));
  return value;
}

Status TimelineSemaphore::signal(std::uint64_t value) {
  // A host signal must advance the counter
  // (VUID-VkSemaphoreSignalInfo-value-03258); refuse one that does not, so a
  // stale value is an error rather than undefined behaviour with layers off.
  // Vulkan has no compare-and-signal, so a signal racing this one from another
  // thread or a queue gets past the check; sync.hpp says how to avoid one.
  VKC_ASSIGN(const std::uint64_t current, this->value());
  if (value <= current) {
    return Status::invalid_argument(
        "TimelineSemaphore::signal: the value must exceed the current one");
  }
  VkSemaphoreSignalInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
  info.semaphore = handle_.get();
  info.value = value;
  VKC_VK_TRY(vkSignalSemaphore(handle_.device(), &info));
  return {};
}

Status TimelineSemaphore::wait(std::uint64_t value,
                               std::uint64_t timeout_ns) const {
  if (!valid()) {
    return Status::invalid_argument(
        "TimelineSemaphore::wait on an empty semaphore");
  }
  VkSemaphore semaphore = handle_.get();
  VkSemaphoreWaitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &semaphore;
  info.pValues = &value;
  const VkResult result = vkWaitSemaphores(handle_.device(), &info, timeout_ns);
  if (result != VK_SUCCESS) return vk_error(result, "vkWaitSemaphores");
  return {};
}

}  // namespace volumetric_kit::core
