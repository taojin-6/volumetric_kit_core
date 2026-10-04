// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/shared_device.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

const char* to_string(QueuePlan plan) noexcept {
  switch (plan) {
    case QueuePlan::TwoQueuesOneFamily:
      return "TwoQueuesOneFamily";
    case QueuePlan::TwoFamilies:
      return "TwoFamilies";
    case QueuePlan::SharedQueue:
      return "SharedQueue";
  }
  return "unknown";
}

SharedDevice::SharedDevice(Instance instance)
    : instance_(std::move(instance)) {}

Result<std::unique_ptr<SharedDevice>> SharedDevice::create(
    const SharedDeviceConfig& config) {
  const bool presents = config.graphics.needs_present;
  if (presents && !config.make_surface) {
    return Status::invalid_argument(
        "SharedDevice::create: the renderer presents, and there is no "
        "make_surface");
  }
  // Neither library is consulted about the other: each states its needs, and
  // the union is what the device must meet.
  VKC_ASSIGN(const DeviceRequirements merged,
             merge(config.compute, config.graphics));

  VKC_ASSIGN(Instance instance, Instance::create(config.instance));
  // Made here, so destruction after any failure below goes through one path.
  std::unique_ptr<SharedDevice> shared(new SharedDevice(std::move(instance)));
  VkInstance vk_instance = shared->instance_.handle();
  if (presents) {
    VKC_ASSIGN(shared->surface_, config.make_surface(vk_instance));
    if (shared->surface_ == VK_NULL_HANDLE) {
      return Status::invalid_argument(
          "SharedDevice::create: make_surface returned no surface");
    }
  }

  // The device-level check -- version, extensions, features, and one family
  // with both libraries' flags that presents -- which names what a device
  // lacks. Every graphics device has a family that does graphics and compute
  // too, so a device that passes has at least the SharedQueue plan.
  VKC_ASSIGN(shared->physical_, shared->instance_.select_physical_device(
                                    merged, shared->surface_));
  const PhysicalDeviceInfo& caps = shared->physical_;
  const std::vector<VkQueueFamilyProperties>& families = caps.queue_families();
  const auto family_count = static_cast<std::uint32_t>(families.size());
  const auto has = [&](std::uint32_t i, VkQueueFlags flags) {
    return (families[i].queueFlags & flags) == flags;
  };
  const auto presents_on = [&](std::uint32_t i) {
    if (!presents) return true;
    VkBool32 supported = VK_FALSE;
    return vkGetPhysicalDeviceSurfaceSupportKHR(
               caps.handle(), i, shared->surface_, &supported) == VK_SUCCESS &&
           supported == VK_TRUE;
  };
  const VkQueueFlags graphics_flags = config.graphics.queue_flags;
  const VkQueueFlags compute_flags = config.compute.queue_flags;
  bool found = false;
  const auto take = [&](QueuePlan plan, std::uint32_t graphics,
                        std::uint32_t compute) {
    shared->plan_ = plan;
    shared->graphics_family_ = graphics;
    shared->compute_family_ = compute;
    found = true;
  };
  // Best first, every plan searched in turn: a device whose families hold a
  // queue each (MoltenVK) has no first plan, and stopping at the first
  // family that does both would take the last plan and lose independent
  // submission.
  for (std::uint32_t i = 0; i < family_count && !found; ++i) {
    if (has(i, graphics_flags | compute_flags) && families[i].queueCount >= 2 &&
        presents_on(i)) {
      take(QueuePlan::TwoQueuesOneFamily, i, i);
    }
  }
  for (std::uint32_t i = 0; i < family_count && !found; ++i) {
    if (!has(i, graphics_flags) || !presents_on(i)) continue;
    for (std::uint32_t j = 0; j < family_count && !found; ++j) {
      if (j != i && has(j, compute_flags)) take(QueuePlan::TwoFamilies, i, j);
    }
  }
  for (std::uint32_t i = 0; i < family_count && !found; ++i) {
    if (has(i, graphics_flags | compute_flags) && presents_on(i)) {
      take(QueuePlan::SharedQueue, i, i);
    }
  }
  if (!found) {
    return Status::unsupported(
        "SharedDevice::create: " + std::string(caps.properties().deviceName) +
        " has no family that presents and does graphics beside a family "
        "that does compute");
  }

  // What the device enables, kept for the payloads to declare.
  shared->extension_storage_ = detail::enabled_extensions(caps, merged);
  shared->extensions_.reserve(shared->extension_storage_.size());
  for (const std::string& name : shared->extension_storage_) {
    shared->extensions_.push_back(name.c_str());
  }
  shared->enabled_ = merged;
  shared->presents_ = presents;
  detail::FeatureChain chain;
  chain.build(merged);

  // A create info per distinct family; two queues from it only under the
  // first plan.
  const float priorities[2] = {1.0F, 1.0F};
  std::vector<VkDeviceQueueCreateInfo> queues;
  VkDeviceQueueCreateInfo queue{};
  queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue.pQueuePriorities = priorities;
  queue.queueFamilyIndex = shared->graphics_family_;
  queue.queueCount = shared->plan_ == QueuePlan::TwoQueuesOneFamily ? 2 : 1;
  queues.push_back(queue);
  if (shared->compute_family_ != shared->graphics_family_) {
    queue.queueFamilyIndex = shared->compute_family_;
    queue.queueCount = 1;
    queues.push_back(queue);
  }
  VkDeviceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.pNext =
      &chain.features2;  // features via features2, not pEnabledFeatures
  info.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
  info.pQueueCreateInfos = queues.data();
  info.enabledExtensionCount =
      static_cast<std::uint32_t>(shared->extensions_.size());
  info.ppEnabledExtensionNames =
      shared->extensions_.empty() ? nullptr : shared->extensions_.data();
  // Created into a local: a failed create leaves the output unspecified.
  VkDevice device = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateDevice(caps.handle(), &info, nullptr, &device));
  shared->device_ = device;
  vkGetDeviceQueue(device, shared->graphics_family_, 0,
                   &shared->graphics_queue_);
  vkGetDeviceQueue(device, shared->compute_family_,
                   shared->plan_ == QueuePlan::TwoQueuesOneFamily ? 1 : 0,
                   &shared->compute_queue_);
  return shared;
}

SharedDevice::~SharedDevice() {
  // vkDestroyDevice needs every queue idle, and neither adopter drains the
  // other's; both must be gone by now, as their wrappers borrow these handles.
  wait_idle();
  if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_.handle(), surface_, nullptr);
  }
  // instance_ goes last, as a member.
}

std::mutex* SharedDevice::compute_mutex() const noexcept {
  return plan_ == QueuePlan::SharedQueue ? &graphics_mutex_ : &compute_mutex_;
}

void SharedDevice::wait_idle() const noexcept {
  if (device_ == VK_NULL_HANDLE) return;
  // Per queue, not vkDeviceWaitIdle: a wait is a queue operation, which must
  // hold the mutex that queue's submits hold.
  const auto drain = [](VkQueue queue, std::mutex* guard) {
    if (queue == VK_NULL_HANDLE) return;
    const std::scoped_lock lock(*guard);
    // A drain that fails (device lost) leaves nothing more to wait for.
    static_cast<void>(vkQueueWaitIdle(queue));
  };
  drain(graphics_queue_, &graphics_mutex_);
  if (compute_queue_ != graphics_queue_) drain(compute_queue_, compute_mutex());
}

VkSurfaceKHR SharedDevice::release_surface() noexcept {
  return std::exchange(surface_, VK_NULL_HANDLE);
}

AdoptedDevice SharedDevice::payload(std::uint32_t family, VkQueue queue,
                                    std::mutex* mutex) const {
  AdoptedDevice adopted;
  adopted.instance = instance_.handle();
  adopted.instance_api_version = instance_.api_version();
  adopted.physical_device = physical_.handle();
  adopted.device = device_;
  adopted.queue_family = family;
  adopted.queue = queue;
  adopted.submit_mutex = mutex;
  // Read back from what vkCreateDevice was given, never restated: adopt
  // checks a library's needs against this declaration, as Vulkan cannot be
  // asked what a logical device enabled.
  adopted.enabled_extensions = extensions_.data();
  adopted.enabled_extension_count =
      static_cast<std::uint32_t>(extensions_.size());
  adopted.enabled_features = enabled_.features;
  adopted.enabled_timeline_semaphore = enabled_.timeline_semaphore;
  adopted.enabled_scalar_block_layout = enabled_.scalar_block_layout;
  adopted.enabled_dynamic_rendering = enabled_.dynamic_rendering;
  adopted.enabled_debug_utils = instance_.debug_utils_enabled();
  return adopted;
}

AdoptedDevice SharedDevice::compute_payload() const {
  return payload(compute_family_, compute_queue_, compute_mutex());
}

AdoptedDevice SharedDevice::graphics_payload() const {
  AdoptedDevice adopted =
      payload(graphics_family_, graphics_queue_, &graphics_mutex_);
  // The family was chosen for its present support, so it presents on its
  // own queue (whose mutex it already holds).
  if (presents_) {
    adopted.has_present = true;
    adopted.present_family = graphics_family_;
    adopted.present_queue = graphics_queue_;
  }
  return adopted;
}

std::string SharedDevice::summary() const {
  std::string line = std::string(physical_.properties().deviceName) + ": " +
                     to_string(plan_) + ", graphics family " +
                     std::to_string(graphics_family_);
  switch (plan_) {
    case QueuePlan::TwoQueuesOneFamily:
      return line + ", 2 queues (graphics + compute)";
    case QueuePlan::TwoFamilies:
      return line + ", compute family " + std::to_string(compute_family_) +
             ", a queue each";
    case QueuePlan::SharedQueue:
      return line + ", 1 shared queue (mutex-guarded; submits serialize)";
  }
  return line;
}

}  // namespace volumetric_kit::core
