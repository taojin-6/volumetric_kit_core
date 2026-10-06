// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/device.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support.hpp"
#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

constexpr const char* kLogSource = "vulkan";
constexpr const char* kExternalMemoryFd =
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
// Its name macro needs VK_USE_PLATFORM_METAL_EXT, which only Metal code
// defines; the string is stable.
constexpr const char* kMetalObjects = "VK_EXT_metal_objects";

// Runs `cleanup` on scope exit unless release()d. Templated on the callable,
// so the destructor calls it directly and nothing on the way can throw.
template <class Cleanup>
class ScopeGuard {
 public:
  explicit ScopeGuard(Cleanup cleanup) : cleanup_(std::move(cleanup)) {}
  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;
  ~ScopeGuard() {
    if (active_) cleanup_();
  }
  void release() noexcept { active_ = false; }

 private:
  Cleanup cleanup_;
  bool active_ = true;
};

// The VkDeviceCreateInfo feature chain create_device builds: the
// requirements' core features, timeline semaphores, scalar block layout and
// dynamic rendering, then the caller's chain. Built in place -- its nodes point
// at one another -- so it is never copied or moved once built. `enabled` is
// what it enables, the caller's chain included.
struct FeatureChain {
  VkPhysicalDeviceFeatures2 features2{};
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
  VkPhysicalDeviceScalarBlockLayoutFeatures scalar{};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic{};
  EnabledFeatures enabled;

  FeatureChain() = default;
  FeatureChain(const FeatureChain&) = delete;
  FeatureChain& operator=(const FeatureChain&) = delete;

  void build(const DeviceRequirements& reqs) {
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features = reqs.features;
    timeline.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;
    scalar.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
    scalar.scalarBlockLayout = VK_TRUE;
    dynamic.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic.dynamicRendering = VK_TRUE;

    // A chain may not hold both a version aggregate and a struct it subsumes
    // (VUID-VkDeviceCreateInfo-pNext-02830). So where the caller's chain
    // already carries the aggregate or the standalone struct for a feature,
    // the bit is raised in the caller's struct, and this chain links its own
    // only otherwise.
    VkPhysicalDeviceVulkan12Features* v12 = nullptr;
    VkPhysicalDeviceVulkan13Features* v13 = nullptr;
    VkPhysicalDeviceTimelineSemaphoreFeatures* their_timeline = nullptr;
    VkPhysicalDeviceScalarBlockLayoutFeatures* their_scalar = nullptr;
    VkPhysicalDeviceDynamicRenderingFeatures* their_dynamic = nullptr;
    for (auto* node = static_cast<VkBaseOutStructure*>(reqs.feature_chain);
         node != nullptr; node = node->pNext) {
      switch (node->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
          v12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
          v13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
          their_timeline =
              reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(
                  node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
          their_scalar =
              reinterpret_cast<VkPhysicalDeviceScalarBlockLayoutFeatures*>(
                  node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
          their_dynamic =
              reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(node);
          break;
        default:
          break;
      }
    }

    enabled.core = reqs.features;
    enabled.timeline_semaphore =
        reqs.timeline_semaphore ||
        (v12 != nullptr && v12->timelineSemaphore == VK_TRUE) ||
        (their_timeline != nullptr &&
         their_timeline->timelineSemaphore == VK_TRUE);
    enabled.scalar_block_layout =
        reqs.scalar_block_layout ||
        (v12 != nullptr && v12->scalarBlockLayout == VK_TRUE) ||
        (their_scalar != nullptr && their_scalar->scalarBlockLayout == VK_TRUE);
    enabled.dynamic_rendering =
        reqs.dynamic_rendering ||
        (v13 != nullptr && v13->dynamicRendering == VK_TRUE) ||
        (their_dynamic != nullptr &&
         their_dynamic->dynamicRendering == VK_TRUE);

    auto* tail = reinterpret_cast<VkBaseOutStructure*>(&features2);
    auto link = [&tail](void* feature) {
      auto* node = static_cast<VkBaseOutStructure*>(feature);
      node->pNext = nullptr;
      tail->pNext = node;
      tail = node;
    };
    if (reqs.timeline_semaphore) {
      if (v12 != nullptr) {
        v12->timelineSemaphore = VK_TRUE;
      } else if (their_timeline != nullptr) {
        their_timeline->timelineSemaphore = VK_TRUE;
      } else {
        link(&timeline);
      }
    }
    if (reqs.scalar_block_layout) {
      if (v12 != nullptr) {
        v12->scalarBlockLayout = VK_TRUE;
      } else if (their_scalar != nullptr) {
        their_scalar->scalarBlockLayout = VK_TRUE;
      } else {
        link(&scalar);
      }
    }
    if (reqs.dynamic_rendering) {
      if (v13 != nullptr) {
        v13->dynamicRendering = VK_TRUE;
      } else if (their_dynamic != nullptr) {
        their_dynamic->dynamicRendering = VK_TRUE;
      } else {
        link(&dynamic);
      }
    }
    tail->pNext = static_cast<VkBaseOutStructure*>(reqs.feature_chain);
  }
};

}  // namespace

namespace detail {

std::vector<std::string> enabled_extensions(const PhysicalDeviceInfo& caps,
                                            const DeviceRequirements& reqs) {
  std::vector<std::string> enabled = required_extensions(reqs);
  auto enable_if_offered = [&](const std::string& name) {
    if (caps.supports_device_extension(name.c_str()) &&
        std::find(enabled.begin(), enabled.end(), name) == enabled.end()) {
      enabled.push_back(name);
    }
  };
  // The spec requires enabling it wherever the device exposes it (MoltenVK).
  enable_if_offered(kPortabilitySubset);
  // The driver's own heap budgets, which an Allocator allocates within.
  enable_if_offered(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  for (const std::string& name : reqs.optional_extensions) {
    enable_if_offered(name);
  }
  return enabled;
}

Result<CreatedDevice> create_device(const PhysicalDeviceInfo& caps,
                                    const DeviceRequirements& reqs,
                                    const std::vector<std::string>& extensions,
                                    const std::vector<QueueRequest>& queues) {
  std::vector<const char*> extension_names;
  extension_names.reserve(extensions.size());
  for (const std::string& name : extensions) {
    extension_names.push_back(name.c_str());
  }

  FeatureChain chain;
  chain.build(reqs);

  std::vector<VkDeviceQueueCreateInfo> infos;
  std::uint32_t most = 0;
  for (const QueueRequest& request : queues) {
    most = std::max(most, request.count);
    auto same = std::find_if(infos.begin(), infos.end(),
                             [&](const VkDeviceQueueCreateInfo& q) {
                               return q.queueFamilyIndex == request.family;
                             });
    if (same != infos.end()) {
      same->queueCount = std::max(same->queueCount, request.count);
      continue;
    }
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = request.family;
    q.queueCount = request.count;
    infos.push_back(q);
  }
  // Every create info reads its priorities from one array, as long as the
  // longest.
  const std::vector<float> priorities(most, 1.0f);
  for (VkDeviceQueueCreateInfo& q : infos) {
    q.pQueuePriorities = priorities.data();
  }

  VkDeviceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.pNext =
      &chain.features2;  // features via features2, not pEnabledFeatures
  info.queueCreateInfoCount = static_cast<std::uint32_t>(infos.size());
  info.pQueueCreateInfos = infos.data();
  info.enabledExtensionCount =
      static_cast<std::uint32_t>(extension_names.size());
  info.ppEnabledExtensionNames =
      extension_names.empty() ? nullptr : extension_names.data();

  // Created into a local: a failed create leaves the output unspecified, and
  // the caller must not destroy whatever it holds.
  VkDevice device = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateDevice(caps.handle(), &info, nullptr, &device));
  return CreatedDevice{device, chain.enabled};
}

}  // namespace detail

Result<Device> Device::create(const Instance& instance,
                              const PhysicalDeviceInfo& physical,
                              const DeviceRequirements& reqs,
                              VkSurfaceKHR surface) {
  // The usable version bounds the requirements by the instance's only when
  // the capabilities were captured on it, or on one of a lower version.
  if (physical.api_version() > instance.api_version()) {
    return Status::invalid_argument(
        "Device::create: the capabilities were captured on an instance of a "
        "higher Vulkan version than this one; query them on this one");
  }
  return create(instance.handle(), instance.debug_utils_enabled(), physical,
                reqs, surface);
}

Result<Device> Device::create(VkInstance instance,
                              bool instance_debug_utils_enabled,
                              const PhysicalDeviceInfo& physical,
                              const DeviceRequirements& reqs,
                              VkSurfaceKHR surface) {
  // `instance` is a lifetime contract: the device stores only handles.
  (void)instance;
  if (physical.handle() == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "Device::create: no physical device captured");
  }
  const PhysicalDeviceInfo& caps = physical;
  VKC_ASSIGN(const DeviceSupport support,
             check_device_support(caps, reqs, surface));

  std::vector<std::string> extensions = detail::enabled_extensions(caps, reqs);
  std::vector<detail::QueueRequest> queues = {{support.queue_family, 1}};
  if (support.present_family) queues.push_back({*support.present_family, 1});
  VKC_ASSIGN(const detail::CreatedDevice created,
             detail::create_device(caps, reqs, extensions, queues));
  Device device;
  device.state_.device = created.device;
  device.state_.enabled = created.enabled;
  device.state_.physical = caps.handle();
  device.state_.owns_device = true;
  device.state_.queue_family = support.queue_family;
  const VkQueueFamilyProperties& family =
      caps.queue_families()[support.queue_family];
  device.state_.queue_flags = family.queueFlags;
  device.state_.timestamp_valid_bits = family.timestampValidBits;
  vkGetDeviceQueue(device.state_.device, support.queue_family, 0,
                   &device.state_.queue);
  if (support.present_family) {
    device.state_.present_family = *support.present_family;
    if (*support.present_family == support.queue_family) {
      device.state_.present_queue = device.state_.queue;
    } else {
      vkGetDeviceQueue(device.state_.device, *support.present_family, 0,
                       &device.state_.present_queue);
    }
  }
  device.caps_ = caps;
  device.enabled_extensions_ = std::move(extensions);
  device.resolve_entry_points(instance_debug_utils_enabled);
  return device;
}

Result<Device> Device::adopt(const AdoptedDevice& adopted,
                             const DeviceRequirements& reqs) {
  if (adopted.instance == VK_NULL_HANDLE ||
      adopted.physical_device == VK_NULL_HANDLE ||
      adopted.device == VK_NULL_HANDLE || adopted.queue == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "Device::adopt: instance, physical device, device and queue must all "
        "be non-null");
  }
  if (adopted.instance_api_version == 0) {
    return Status::invalid_argument(
        "Device::adopt: instance_api_version is unset; declare the version "
        "the instance was created with (VkApplicationInfo::apiVersion)");
  }
  if (adopted.has_present && adopted.present_queue == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "Device::adopt: has_present is set without a present queue");
  }
  if (reqs.needs_present && !adopted.has_present) {
    return Status::invalid_argument(
        "Device::adopt: the requirements need presentation, and no present "
        "queue was assigned");
  }

  PhysicalDeviceInfo caps = PhysicalDeviceInfo::query(
      adopted.physical_device, adopted.instance_api_version);
  const std::vector<VkQueueFamilyProperties>& families = caps.queue_families();
  if (adopted.queue_family >= families.size() ||
      (adopted.has_present && adopted.present_family >= families.size())) {
    return Status::invalid_argument(
        "Device::adopt: a queue family is out of range for the physical "
        "device");
  }
  const VkQueueFamilyProperties& family = families[adopted.queue_family];
  if ((family.queueFlags & reqs.queue_flags) != reqs.queue_flags) {
    return Status::unsupported(
        "Device::adopt: the assigned queue family lacks a required capability "
        "(DeviceRequirements::queue_flags)");
  }
  // Supported by the physical device -- authoritative, and queryable...
  const std::vector<std::string> required = detail::required_extensions(reqs);
  VKC_TRY(detail::check_physical_support(caps, reqs, required)
              .with_context("Device::adopt"));
  // ...and declared enabled by the creator, as Vulkan cannot be asked what a
  // logical device enabled: recorded below, and checked as the record a
  // later check_enabled reads.
  std::vector<std::string> declared;
  if (adopted.enabled_extensions != nullptr) {
    for (std::uint32_t i = 0; i < adopted.enabled_extension_count; ++i) {
      if (adopted.enabled_extensions[i] != nullptr) {
        declared.emplace_back(adopted.enabled_extensions[i]);
      }
    }
  }

  Device device;
  device.state_.physical = adopted.physical_device;
  device.state_.device = adopted.device;
  device.state_.owns_device = false;  // borrowed: destroy() leaves it alone
  device.state_.queue = adopted.queue;
  device.state_.queue_family = adopted.queue_family;
  // Read from the driver, not declared: a family's capabilities are
  // queryable, so there is nothing for a hand-written payload to get wrong.
  device.state_.queue_flags = family.queueFlags;
  device.state_.timestamp_valid_bits = family.timestampValidBits;
  device.state_.submit_mutex = adopted.submit_mutex;
  if (adopted.has_present) {
    device.state_.present_queue = adopted.present_queue;
    device.state_.present_family = adopted.present_family;
    device.state_.present_mutex = adopted.present_mutex;
  }
  device.state_.enabled = adopted.enabled_features;
  device.caps_ = std::move(caps);
  device.enabled_extensions_ = std::move(declared);
  VKC_TRY(device.check_record(reqs, required).with_context("Device::adopt"));
  device.resolve_entry_points(adopted.enabled_debug_utils);
  return device;
}

Status Device::check_enabled(const DeviceRequirements& reqs) const {
  if (state_.device == VK_NULL_HANDLE) {
    return Status::invalid_argument("Device::check_enabled: an empty device");
  }
  if (reqs.feature_chain != nullptr) {
    return Status::invalid_argument(
        "Device::check_enabled: a device keeps no record of a feature chain, "
        "so DeviceRequirements::feature_chain cannot be checked; check its "
        "features by other means, and pass the requirements without it");
  }
  if ((state_.queue_flags & reqs.queue_flags) != reqs.queue_flags) {
    return Status::unsupported(
        "Device::check_enabled: the device's queue family lacks a required "
        "capability (DeviceRequirements::queue_flags)");
  }
  if (reqs.needs_present && !has_present()) {
    return Status::unsupported(
        "Device::check_enabled: the device has no present queue");
  }
  // What create and adopt hold requirements to: a record -- an adopted
  // device's declaration -- may claim a feature the physical device lacks,
  // or one its usable version does not make core.
  const std::vector<std::string> required = detail::required_extensions(reqs);
  VKC_TRY(detail::check_physical_support(caps_, reqs, required)
              .with_context("Device::check_enabled"));
  return check_record(reqs, required).with_context("Device::check_enabled");
}

Status Device::check_record(const DeviceRequirements& reqs,
                            const std::vector<std::string>& required) const {
  // An adopted device's record is its creator's declaration, so a refusal
  // names the field to fix.
  const bool declared = !state_.owns_device;
  const auto missing = [declared](const std::string& what, const char* field) {
    return Status::unsupported(
        declared
            ? what + " is not declared enabled (AdoptedDevice::" + field + ")"
            : what + " was not enabled when the device was created");
  };
  for (const std::string& name : required) {
    if (!extension_enabled(name.c_str())) {
      return missing("the required extension " + name, "enabled_extensions");
    }
  }
  if (const char* feature =
          detail::first_missing_feature(reqs.features, state_.enabled.core)) {
    return missing(std::string("the core feature ") + feature,
                   "enabled_features.core");
  }
  struct Flag {
    bool wanted;
    bool enabled;
    const char* feature;
    const char* field;
  };
  for (const Flag& flag :
       {Flag{reqs.timeline_semaphore, state_.enabled.timeline_semaphore,
             "timelineSemaphore", "enabled_features.timeline_semaphore"},
        Flag{reqs.scalar_block_layout, state_.enabled.scalar_block_layout,
             "scalarBlockLayout", "enabled_features.scalar_block_layout"},
        Flag{reqs.dynamic_rendering, state_.enabled.dynamic_rendering,
             "dynamicRendering", "enabled_features.dynamic_rendering"}}) {
    if (flag.wanted && !flag.enabled) return missing(flag.feature, flag.field);
  }
  return {};
}

void Device::resolve_entry_points(bool instance_debug_utils) noexcept {
  // Only on the caller's word that the instance enabled the extension:
  // vkGetDeviceProcAddr for a command of an extension that was not enabled is
  // not portable (a conformant loader returns null, a directly linked
  // MoltenVK a live pointer that must not be called). All three or none, so
  // a half-resolved driver cannot open a label it cannot close.
  if (instance_debug_utils) {
    auto set_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(state_.device, "vkSetDebugUtilsObjectNameEXT"));
    auto begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(state_.device, "vkCmdBeginDebugUtilsLabelEXT"));
    auto end = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(state_.device, "vkCmdEndDebugUtilsLabelEXT"));
    if (set_name != nullptr && begin != nullptr && end != nullptr) {
      state_.set_object_name = set_name;
      state_.begin_label = begin;
      state_.end_label = end;
    }
  }
  // Only where enabled, for the same reason.
  if (extension_enabled(kExternalMemoryFd)) {
    state_.get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(state_.device, "vkGetMemoryFdKHR"));
  }
  state_.metal_objects = extension_enabled(kMetalObjects);
}

Device::Device(Device&& other) noexcept
    : state_(std::exchange(other.state_, State{})),
      caps_(std::exchange(other.caps_, PhysicalDeviceInfo{})),
      enabled_extensions_(std::exchange(other.enabled_extensions_, {})),
      made_(std::exchange(other.made_, {})),
      free_commands_(std::exchange(other.free_commands_, {})) {}

Device& Device::operator=(Device&& other) noexcept {
  if (this != &other) {
    destroy();
    state_ = std::exchange(other.state_, State{});
    caps_ = std::exchange(other.caps_, PhysicalDeviceInfo{});
    enabled_extensions_ = std::exchange(other.enabled_extensions_, {});
    made_ = std::exchange(other.made_, {});
    free_commands_ = std::exchange(other.free_commands_, {});
  }
  return *this;
}

Device::~Device() { destroy(); }

void Device::destroy() noexcept {
  bool unfinished = false;
  for (Command& command : made_) {
    // One left to the device after a failed wait is waited for first, and
    // leaked if the device may still run it. A lost device's is freed: its
    // children must still be destroyed.
    if (command.pending) {
      const VkResult waited = vkWaitForFences(state_.device, 1, &command.fence,
                                              VK_TRUE, UINT64_MAX);
      if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) {
        unfinished = true;
        // What the work uses is leaked with it: freeing memory a running
        // submit may read is undefined, where a leak is only a leak.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        static_cast<void>(new (std::nothrow) std::shared_ptr<void>(
            std::move(command.keep_alive)));
        continue;
      }
    }
    // Before the VkDevice: a staging buffer frees through its allocator, which
    // it may be the last to hold.
    command.keep_alive.reset();
    vkDestroyFence(state_.device, command.fence, nullptr);
    if (command.pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(state_.device, command.pool, nullptr);  // + buffer
    }
  }
  made_.clear();
  free_commands_.clear();
  if (state_.device != VK_NULL_HANDLE && state_.owns_device) {
    if (unfinished) {
      // Destroying it would destroy a device that may still run that work,
      // with the work's fence and pool alive (VUID-vkDestroyDevice-device-
      // 05137): undefined, where a leak is only a leak.
      log_message(LogLevel::Error, kLogSource,
                  "Device: a submit whose wait failed may still be running; "
                  "its VkDevice is leaked rather than destroyed under it");
    } else {
      vkDestroyDevice(state_.device, nullptr);
    }
  }
  state_ = State{};
  caps_ = PhysicalDeviceInfo{};
  enabled_extensions_.clear();
}

bool Device::extension_enabled(const char* name) const {
  if (name == nullptr) return false;
  return std::find(enabled_extensions_.begin(), enabled_extensions_.end(),
                   name) != enabled_extensions_.end();
}

std::mutex* Device::present_mutex() const noexcept {
  // One queue for both: presents and submits share its mutex.
  if (state_.present_queue == state_.queue) return submit_mutex();
  return state_.present_mutex != nullptr ? state_.present_mutex
                                         : &present_queue_mutex_;
}

VkResult Device::queue_submit(std::uint32_t count, const VkSubmitInfo* submits,
                              VkFence fence) const {
  const std::scoped_lock lock(*submit_mutex());
  return vkQueueSubmit(state_.queue, count, submits, fence);
}

VkResult Device::queue_present(const VkPresentInfoKHR& present_info) const {
  VKC_CHECK(has_present(), "Device::queue_present without a present queue");
  const std::scoped_lock lock(*present_mutex());
  return vkQueuePresentKHR(state_.present_queue, &present_info);
}

Status Device::wait_idle() const {
  {
    const std::scoped_lock lock(*submit_mutex());
    VKC_VK_TRY(vkQueueWaitIdle(state_.queue));
  }
  if (has_present() && state_.present_queue != state_.queue) {
    const std::scoped_lock lock(*present_mutex());
    VKC_VK_TRY(vkQueueWaitIdle(state_.present_queue));
  }
  return {};
}

Result<Device::Command> Device::take_command(bool record) const {
  const std::scoped_lock lock(commands_mutex_);
  Command command;
  if (!free_commands_.empty()) {
    // One that has a buffer if this submit records, and preferably none if
    // it does not, so the buffers stay with the submits that record.
    auto fits = std::find_if(
        free_commands_.begin(), free_commands_.end(),
        [&](const Command& c) { return (c.pool != VK_NULL_HANDLE) == record; });
    if (fits == free_commands_.end()) fits = free_commands_.begin();
    command = *fits;
    free_commands_.erase(fits);
  } else {
    // Every one is in use, so make another -- only as often as submits
    // overlap. Both lists grow first, so give_back never allocates.
    made_.reserve(made_.size() + 1);
    free_commands_.reserve(made_.size() + 1);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VKC_VK_TRY(
        vkCreateFence(state_.device, &fence_info, nullptr, &command.fence));
    made_.push_back(command);
  }
  if (record && command.pool == VK_NULL_HANDLE) {
    const Status added = add_command_buffer(&command);
    if (!added) {
      free_commands_.push_back(command);  // its fence is still good
      return added;
    }
  }
  return command;
}

Status Device::add_command_buffer(Command* command) const {
  // The pool resets its buffer on the next begin (RESET_COMMAND_BUFFER).
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = state_.queue_family;
  VkCommandPool pool = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateCommandPool(state_.device, &pool_info, nullptr, &pool));
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  const VkResult allocated =
      vkAllocateCommandBuffers(state_.device, &alloc, &buffer);
  if (allocated != VK_SUCCESS) {
    vkDestroyCommandPool(state_.device, pool, nullptr);
    return vk_error(allocated, "vkAllocateCommandBuffers");
  }
  command->pool = pool;
  command->buffer = buffer;
  for (Command& made : made_) {
    if (made.fence == command->fence) made = *command;
  }
  return {};
}

// take_command reserved room in free_commands_ for every command made, so the
// push_back never allocates and cannot throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void Device::give_back(const Command& command) const noexcept {
  const std::scoped_lock lock(commands_mutex_);
  free_commands_.push_back(command);
}

void Device::leave_to_device(const Command& command,
                             std::shared_ptr<void> keep_alive) const noexcept {
  const std::scoped_lock lock(commands_mutex_);
  for (Command& made : made_) {
    if (made.fence == command.fence) {  // one each: the fence names it
      made.pending = true;
      made.keep_alive = std::move(keep_alive);
      break;
    }
  }
}

Status Device::submit_waiting(VkCommandBuffer cmd, const Command& command,
                              std::shared_ptr<void> keep_alive,
                              bool* reusable) const {
  *reusable = true;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  // A failed submit leaves the buffer and fence as they were -- except one
  // that loses the device, which promises nothing, so it is treated as a
  // failed wait.
  const VkResult submitted = queue_submit(1, &submit, command.fence);
  if (submitted != VK_SUCCESS && submitted != VK_ERROR_DEVICE_LOST) {
    return vk_error(submitted, "vkQueueSubmit");
  }
  const VkResult waited =
      submitted == VK_SUCCESS
          ? vkWaitForFences(state_.device, 1, &command.fence, VK_TRUE,
                            UINT64_MAX)
          : submitted;
  if (waited != VK_SUCCESS) {
    // The submit may still be pending, so its buffer and fence must not go to
    // another submit, nor what it uses be freed; destroy() waits for them
    // before freeing them.
    leave_to_device(command, std::move(keep_alive));
    *reusable = false;
    return vk_error(
        waited, submitted == VK_SUCCESS ? "vkWaitForFences" : "vkQueueSubmit");
  }
  // One that will not reset is not reused; destroy() still frees it.
  if (vkResetFences(state_.device, 1, &command.fence) != VK_SUCCESS) {
    *reusable = false;
  }
  return {};
}

Status Device::submit_single_time(
    const std::function<void(VkCommandBuffer)>& record,
    std::shared_ptr<void> keep_alive, bool* in_flight) const {
  if (in_flight != nullptr) *in_flight = false;
  VKC_ASSIGN(const Command command, take_command(/*record=*/true));
  VkCommandBuffer cmd = command.buffer;
  bool recording = false;
  ScopeGuard give_back_command([&] {
    // A buffer abandoned mid-recording cannot be begun again until reset;
    // one that will not reset is not reused.
    if (recording && vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return;
    give_back(command);
  });

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC_VK_TRY(vkBeginCommandBuffer(cmd, &begin));
  recording = true;
  record(cmd);
  VKC_VK_TRY(vkEndCommandBuffer(cmd));
  recording = false;

  bool reusable = true;
  Status status =
      submit_waiting(cmd, command, std::move(keep_alive), &reusable);
  if (!reusable) give_back_command.release();
  // Failed and not reusable: left to the device, which may still run it. (A
  // fence that will not reset is not reusable either, but after success.)
  if (in_flight != nullptr) *in_flight = !status.ok() && !reusable;
  return status;
}

Status Device::submit_single_time(
    const std::function<void(VkCommandBuffer)>& record, GpuStageScope& stage,
    std::shared_ptr<void> keep_alive) const {
  const GpuSpanTag tag = stage.tag();
  if (tag.timer == nullptr || !tag.timer->available()) {
    return submit_single_time(record, std::move(keep_alive));
  }
  GpuTimer& timer = *tag.timer;
  // The pool goes with what the work uses, so the device holds it past a
  // failed wait.
  auto kept =
      std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(
          std::move(keep_alive), timer.keep_alive());
  std::uint32_t span = GpuTimer::kNoSpan;
  // A record that throws leaves a span whose command buffer never ran.
  ScopeGuard drop_span([&] { timer.discard(span); });
  bool in_flight = false;
  const Status submitted = submit_single_time(
      [&](VkCommandBuffer cmd) {
        span = timer.begin(cmd, tag);
        record(cmd);
        timer.end(cmd, span);
      },
      std::move(kept), &in_flight);
  drop_span.release();
  if (span != GpuTimer::kNoSpan) timer.settle(span, 1, submitted, in_flight);
  return submitted;
}

namespace {

// The refusals submit_pending makes before it takes a command buffer.
Status check_timeline_points(const Device& device,
                             const std::vector<TimelinePoint>& wait,
                             const std::vector<TimelinePoint>& signal) {
  if (wait.empty() && signal.empty()) return {};
  for (const std::vector<TimelinePoint>* points : {&wait, &signal}) {
    for (const TimelinePoint& point : *points) {
      if (point.semaphore == nullptr || !point.semaphore->valid()) {
        return Status::invalid_argument(
            "Device::submit_pending: a timeline semaphore is null or empty");
      }
    }
  }
  // The feature alone, as TimelineSemaphore::create asks for it: a device
  // adopted without it may share a VkDevice whose semaphores another library
  // made.
  DeviceRequirements timeline;
  timeline.api_version = VK_API_VERSION_1_0;
  timeline.queue_flags = 0;
  timeline.timeline_semaphore = true;
  VKC_TRY(
      device.check_enabled(timeline).with_context("Device::submit_pending"));
  for (const std::vector<TimelinePoint>* points : {&wait, &signal}) {
    for (const TimelinePoint& point : *points) {
      if (point.semaphore->device() != device.handle()) {
        return Status::invalid_argument(
            "Device::submit_pending: a timeline semaphore was made on another "
            "VkDevice");
      }
    }
  }
  // A value to set must advance its counter
  // (VUID-VkSubmitInfo-pSignalSemaphores-03242). As for
  // TimelineSemaphore::signal, this catches a stale value, not a race.
  for (const TimelinePoint& point : signal) {
    VKC_ASSIGN(const std::uint64_t current, point.semaphore->value());
    if (point.value <= current) {
      return Status::invalid_argument(
          "Device::submit_pending: a value to set must exceed its semaphore's "
          "current one");
    }
  }
  return {};
}

}  // namespace

Result<Device::PendingSubmit> Device::submit_pending(
    const std::function<void(VkCommandBuffer)>& record,
    const std::vector<TimelinePoint>& wait,
    const std::vector<TimelinePoint>& signal,
    std::shared_ptr<void> keep_alive) const {
  if (state_.device == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "Device::submit_pending: the device is moved-from");
  }
  VKC_TRY(check_timeline_points(*this, wait, signal));
  std::vector<VkSemaphore> wait_semaphores;
  std::vector<std::uint64_t> wait_values;
  for (const TimelinePoint& point : wait) {
    wait_semaphores.push_back(point.semaphore->handle());
    wait_values.push_back(point.value);
  }
  // Nothing recorded starts before the values are reached: every stage, the
  // one mask valid on a queue of any capabilities.
  const std::vector<VkPipelineStageFlags> wait_stages(
      wait.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  std::vector<VkSemaphore> signal_semaphores;
  std::vector<std::uint64_t> signal_values;
  for (const TimelinePoint& point : signal) {
    signal_semaphores.push_back(point.semaphore->handle());
    signal_values.push_back(point.value);
  }

  VKC_ASSIGN(const Command command, take_command(/*record=*/true));
  VkCommandBuffer cmd = command.buffer;
  bool recording = false;
  ScopeGuard give_back_command([&] {
    // As submit_single_time's: a buffer abandoned mid-recording is reset.
    if (recording && vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return;
    give_back(command);
  });
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC_VK_TRY(vkBeginCommandBuffer(cmd, &begin));
  recording = true;
  record(cmd);
  VKC_VK_TRY(vkEndCommandBuffer(cmd));
  recording = false;

  VkTimelineSemaphoreSubmitInfo values{};
  values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  values.waitSemaphoreValueCount = static_cast<std::uint32_t>(wait.size());
  values.pWaitSemaphoreValues = wait_values.data();
  values.signalSemaphoreValueCount = static_cast<std::uint32_t>(signal.size());
  values.pSignalSemaphoreValues = signal_values.data();
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  // Only with values to pass: a device without timeline semaphores gets the
  // plain submit submit_single_time makes.
  if (!wait.empty() || !signal.empty()) submit.pNext = &values;
  submit.waitSemaphoreCount = static_cast<std::uint32_t>(wait.size());
  submit.pWaitSemaphores = wait_semaphores.data();
  submit.pWaitDstStageMask = wait_stages.data();
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  submit.signalSemaphoreCount = static_cast<std::uint32_t>(signal.size());
  submit.pSignalSemaphores = signal_semaphores.data();
  const VkResult submitted = queue_submit(1, &submit, command.fence);
  // A refused submit leaves the buffer and fence as they were, for the guard
  // to give back.
  if (submitted != VK_SUCCESS && submitted != VK_ERROR_DEVICE_LOST) {
    return vk_error(submitted, "vkQueueSubmit");
  }
  give_back_command.release();
  if (submitted == VK_ERROR_DEVICE_LOST) {
    // One that loses the device promises nothing: a failed wait.
    leave_to_device(command, std::move(keep_alive));
    return vk_error(submitted, "vkQueueSubmit");
  }
  PendingSubmit pending;
  pending.device_ = this;
  pending.command_ = command;
  pending.keep_alive_ = std::move(keep_alive);
  pending.running_ = true;
  return pending;
}

Device::PendingSubmit::~PendingSubmit() { finish(); }

Device::PendingSubmit::PendingSubmit(PendingSubmit&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      command_(std::exchange(other.command_, Command{})),
      keep_alive_(std::exchange(other.keep_alive_, nullptr)),
      running_(std::exchange(other.running_, false)),
      failed_(std::exchange(other.failed_, Status{})) {}

Device::PendingSubmit& Device::PendingSubmit::operator=(
    PendingSubmit&& other) noexcept {
  if (this != &other) {
    finish();
    device_ = std::exchange(other.device_, nullptr);
    command_ = std::exchange(other.command_, Command{});
    keep_alive_ = std::exchange(other.keep_alive_, nullptr);
    running_ = std::exchange(other.running_, false);
    failed_ = std::exchange(other.failed_, Status{});
  }
  return *this;
}

// Only a failure's message allocates, so only memory exhaustion can throw,
// and a destructor has no caller to report it to: terminating is the answer.
// NOLINTNEXTLINE(bugprone-exception-escape)
void Device::PendingSubmit::finish() noexcept {
  if (!running_) return;
  const Status waited = wait();
  if (!waited.ok()) {
    log_message(LogLevel::Error, kLogSource,
                "Device::PendingSubmit: the wait on destruction failed (" +
                    waited.message() + "); the work is left to the device");
  }
}

bool Device::PendingSubmit::ready() const {
  if (!running_) return true;
  return vkGetFenceStatus(device_->handle(), command_.fence) != VK_NOT_READY;
}

Status Device::PendingSubmit::wait(std::uint64_t timeout_ns) {
  if (!running_) return failed_;
  const VkResult waited = vkWaitForFences(device_->handle(), 1, &command_.fence,
                                          VK_TRUE, timeout_ns);
  if (waited == VK_TIMEOUT) return vk_error(waited, "vkWaitForFences");
  running_ = false;
  if (waited != VK_SUCCESS) {
    // The work may still run, so its buffer and fence must not go to another
    // submit, nor what it uses be freed: the device's, as a failed wait of
    // submit_single_time leaves them.
    device_->leave_to_device(command_, std::move(keep_alive_));
    failed_ = vk_error(waited, "vkWaitForFences");
  } else if (vkResetFences(device_->handle(), 1, &command_.fence) ==
             VK_SUCCESS) {
    device_->give_back(command_);
  }  // One whose fence will not reset is not reused; destroy() frees it.
  keep_alive_.reset();
  command_ = Command{};
  device_ = nullptr;
  return failed_;
}

Status Device::submit_and_wait(VkCommandBuffer cmd) const {
  if (cmd == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "Device::submit_and_wait: null command "
        "buffer");
  }
  // Only the command's fence is used: one without a buffer when one is
  // free, and never a new buffer.
  VKC_ASSIGN(const Command command, take_command(/*record=*/false));
  ScopeGuard give_back_command([&] { give_back(command); });
  bool reusable = true;
  Status status = submit_waiting(cmd, command, nullptr, &reusable);
  if (!reusable) give_back_command.release();
  return status;
}

void Device::set_object_name(VkObjectType type, std::uint64_t handle,
                             const char* name) const noexcept {
  if (state_.set_object_name == nullptr || name == nullptr || handle == 0) {
    return;
  }
  VkDebugUtilsObjectNameInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  info.objectType = type;
  info.objectHandle = handle;
  info.pObjectName = name;
  // Dropped deliberately: naming is a diagnostic, and a driver that refuses
  // one must not fail the work around it.
  (void)state_.set_object_name(state_.device, &info);
}

void Device::begin_debug_label(VkCommandBuffer cmd,
                               const char* name) const noexcept {
  if (state_.begin_label == nullptr || cmd == VK_NULL_HANDLE ||
      name == nullptr) {
    return;
  }
  VkDebugUtilsLabelEXT label{};
  label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
  label.pLabelName = name;
  state_.begin_label(cmd, &label);
}

void Device::end_debug_label(VkCommandBuffer cmd,
                             const char* name) const noexcept {
  // The same conditions begin_debug_label tests, the null name included, so
  // a skipped begin is never closed.
  if (state_.begin_label == nullptr || state_.end_label == nullptr ||
      cmd == VK_NULL_HANDLE || name == nullptr) {
    return;
  }
  state_.end_label(cmd);
}

VkResult Device::memory_fd(VkDeviceMemory memory, int* fd) const noexcept {
  if (state_.get_memory_fd == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                  nullptr, memory,
                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
  return state_.get_memory_fd(state_.device, &info, fd);
}

}  // namespace volumetric_kit::core
