// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/shared_device.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "queue_plan.hpp"
#include "support.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
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
  if (config.compute.needs_present) {
    return Status::invalid_argument(
        "SharedDevice::create: the compute library's queue does not present; "
        "set needs_present on the renderer's requirements");
  }
  const bool presents = config.graphics.needs_present;
  if (presents && !config.make_surface) {
    return Status::invalid_argument(
        "SharedDevice::create: the renderer presents, and there is no "
        "make_surface");
  }
  // Each queue does its library's job whatever the requirements say: they
  // default to compute alone, and a renderer handed a compute family that
  // presents could record no draw on it.
  DeviceRequirements compute = config.compute;
  compute.queue_flags |= VK_QUEUE_COMPUTE_BIT;
  DeviceRequirements graphics = config.graphics;
  graphics.queue_flags |= VK_QUEUE_GRAPHICS_BIT;
  // Neither library is consulted about the other: each states its needs, and
  // the union is what the device must meet.
  VKC_ASSIGN(const DeviceRequirements merged, merge(compute, graphics));

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

  // The device-level check -- version, extensions, features, a family with
  // both libraries' flags, and one that presents -- names what a device
  // lacks, but cannot say those families carve into the two queues: the
  // renderer's must present itself. So each device that passes is asked for
  // its plan too, one with none is refused for that, and the best of the
  // rest is taken.
  std::vector<std::pair<VkPhysicalDevice, detail::QueueCarving>> carvings;
  const auto carve = [&](const PhysicalDeviceInfo& caps) -> Status {
    const std::vector<VkQueueFamilyProperties>& families =
        caps.queue_families();
    // Probed once per family, for every plan to read.
    std::vector<bool> presents_on(families.size(), true);
    if (presents) {
      for (std::uint32_t i = 0; i < families.size(); ++i) {
        presents_on[i] = detail::can_present(caps, i, shared->surface_);
      }
    }
    const std::optional<detail::QueueCarving> carving =
        detail::choose_queue_plan(families, graphics.queue_flags,
                                  compute.queue_flags, presents_on);
    if (!carving) {
      return Status::unsupported(std::string(caps.properties().deviceName) +
                                 " has no family that does graphics" +
                                 (presents ? " and presents" : "") +
                                 " beside a family that does compute");
    }
    carvings.emplace_back(caps.handle(), *carving);
    return {};
  };
  VKC_ASSIGN(shared->physical_,
             detail::select_physical_device(shared->instance_, merged,
                                            shared->surface_, carve));
  const PhysicalDeviceInfo& caps = shared->physical_;
  for (const auto& [device, carving] : carvings) {
    if (device != caps.handle()) continue;
    shared->plan_ = carving.plan;
    shared->graphics_family_ = carving.graphics_family;
    shared->compute_family_ = carving.compute_family;
  }

  // What the device enables, kept for the payloads to declare.
  shared->extension_storage_ = detail::enabled_extensions(caps, merged);
  shared->extensions_.reserve(shared->extension_storage_.size());
  for (const std::string& name : shared->extension_storage_) {
    shared->extensions_.push_back(name.c_str());
  }
  shared->presents_ = presents;

  // Two queues from the renderer's family only under the first plan, where
  // the compute library's is its second.
  const bool two_queues = shared->plan_ == QueuePlan::TwoQueuesOneFamily;
  detail::EnabledFeatures features;
  VKC_ASSIGN(
      shared->device_,
      detail::create_device(caps, merged, shared->extension_storage_,
                            {{shared->graphics_family_, two_queues ? 2U : 1U},
                             {shared->compute_family_, 1}},
                            &features));
  shared->enabled_features_ = features.features;
  shared->enabled_timeline_semaphore_ = features.timeline_semaphore;
  shared->enabled_scalar_block_layout_ = features.scalar_block_layout;
  shared->enabled_dynamic_rendering_ = features.dynamic_rendering;
  vkGetDeviceQueue(shared->device_, shared->graphics_family_, 0,
                   &shared->graphics_queue_);
  vkGetDeviceQueue(shared->device_, shared->compute_family_, two_queues ? 1 : 0,
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
  adopted.enabled_features = enabled_features_;
  adopted.enabled_timeline_semaphore = enabled_timeline_semaphore_;
  adopted.enabled_scalar_block_layout = enabled_scalar_block_layout_;
  adopted.enabled_dynamic_rendering = enabled_dynamic_rendering_;
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
