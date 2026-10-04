// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file shared_device.hpp
/// @brief One Vulkan device satisfying a compute library and a renderer, for
///        each to adopt: the embedder's half of the create/adopt seam.
///
/// A `VkBuffer` is valid only on the `VkDevice` that made it, so the zero-copy
/// handoff -- recon's mesh drawn by gfx in place -- needs one device; two on
/// the same GPU would still cross host memory. Each library publishes its
/// @ref DeviceRequirements, this satisfies their union, and each adopts the
/// device through @ref Device::adopt. It replaces recon's
/// `examples/viewer/shared_device.hpp` and ios's `SharedDevice`, which did the
/// same job twice and differed only in how the surface was made.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief How the two libraries' queues were carved out of the shared device,
///        best first: @ref SharedDevice::create takes the first plan the
///        hardware allows, and each step down costs something real.
enum class QueuePlan {
  /// One family, two queues: independent submission, and no queue-family
  /// ownership transfer on a buffer one library writes and the other reads.
  TwoQueuesOneFamily,
  /// Two families, a queue each: independent submission, but a shared buffer
  /// names both families (`VK_SHARING_MODE_CONCURRENT`) or is transferred.
  /// A driver that reports several families of one queue each lands here.
  TwoFamilies,
  /// One family, one queue shared under one mutex. The last resort: the two
  /// libraries' submits serialize, and neither may submit a GPU-side wait on
  /// a value the other has yet to signal -- a drain holding the mutex
  /// (`Device::wait_idle`) would deadlock against it. Readiness is checked on
  /// the host instead.
  SharedQueue,
};

/// @brief Name a queue plan, for a log or a read-out.
/// @param plan  A plan.
/// @return Its name, e.g. `"TwoFamilies"`.
VKC_VULKAN_API const char* to_string(QueuePlan plan) noexcept;

/// @brief What @ref SharedDevice::create builds from.
///
/// @warning Like @ref Device::create, `create` writes to the structs either
///          side's @ref DeviceRequirements::feature_chain points at, raising
///          the bits it enables in them, though it takes the config `const`:
///          they must be mutable, and a chain reused afterwards carries those
///          bits.
///
/// @code
/// SharedDeviceConfig config;
/// config.instance.app_name = "viewer";
/// for (const char* ext : glfw_instance_extensions) {
///   config.instance.extensions.push_back(ext);
/// }
/// config.compute = fusion_requirements;     // recon's
/// config.graphics = renderer_requirements;  // gfx's, needs_present
/// config.make_surface = [window](VkInstance vk) -> Result<VkSurfaceKHR> {
///   VkSurfaceKHR surface = VK_NULL_HANDLE;
///   VKC_VK_TRY(glfwCreateWindowSurface(vk, window, nullptr, &surface));
///   return surface;
/// };
/// @endcode
struct SharedDeviceConfig {
  /// The instance's configuration: its name, validation, and the instance
  /// extensions the surface needs (`VK_KHR_surface` and the platform's).
  InstanceConfig instance;
  /// The compute library's requirements (recon's): its queue does compute,
  /// whether or not `queue_flags` says so. Its queue never presents, so
  /// `needs_present` is refused here.
  DeviceRequirements compute;
  /// The renderer's requirements (gfx's): its queue does graphics, whether or
  /// not `queue_flags` says so (they default to compute alone), and presents
  /// when `needs_present` is set.
  DeviceRequirements graphics;
  /// Makes the surface the renderer presents to, on the instance it is
  /// given; required when `graphics.needs_present` is set, unused otherwise.
  /// Made before the device, as choosing one tests present support on it.
  std::function<Result<VkSurfaceKHR>(VkInstance)> make_surface;
};

/// @brief One instance, device and (optionally) surface satisfying both
///        libraries, and the payload each adopts it through.
///
/// Owns them all and destroys them in reverse order, after draining every
/// queue it handed out. Every queue has a mutex, handed out with it: Vulkan
/// requires every host operation on a queue be externally synchronized, and
/// @ref wait_idle is a third thread touching both. Under
/// @ref QueuePlan::SharedQueue the two payloads share one mutex. Pinned in
/// memory (made only as a `unique_ptr`), as both libraries keep the mutexes'
/// addresses.
///
/// @warning Both adopted `Device`s, and everything made on them, must be
///          destroyed before this.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<SharedDevice> shared,
///            SharedDevice::create(config));
/// VKC_ASSIGN(Device fusion,
///            Device::adopt(shared->compute_payload(), config.compute));
/// VKC_ASSIGN(Device renderer,
///            Device::adopt(shared->graphics_payload(), config.graphics));
/// log(shared->summary());
/// // "<device name>: TwoFamilies, graphics family 0, compute family 1,
/// // a queue each"
/// @endcode
class VKC_VULKAN_API SharedDevice {
 public:
  /// @brief Build the instance, the surface and the device from both
  ///        libraries' merged requirements.
  ///
  /// The device is the best one, as @ref Instance::select_physical_device
  /// ranks them, that meets the union and has a @ref QueuePlan: a family
  /// that does graphics and presents to the surface itself, beside or
  /// together with one that does compute. One that meets the union but has
  /// no plan is passed over, and the next tried.
  /// @param config  The instance, both requirements and the surface maker.
  ///                Read only, but for the feature structs a
  ///                @ref DeviceRequirements::feature_chain points at.
  /// @return The shared device; @ref Status::Code::InvalidArgument for a
  ///         renderer that presents with no surface maker, a compute library
  ///         that asks to present, or requirements that cannot merge; the
  ///         surface maker's failure; or why no device satisfies the union and
  ///         has a plan, naming what each lacks. Treat a failure as fatal
  ///         rather than falling back to two devices, which would give up the
  ///         zero-copy seam this exists for.
  static Result<std::unique_ptr<SharedDevice>> create(
      const SharedDeviceConfig& config);

  ~SharedDevice();
  SharedDevice(const SharedDevice&) = delete;
  SharedDevice& operator=(const SharedDevice&) = delete;
  SharedDevice(SharedDevice&&) = delete;
  SharedDevice& operator=(SharedDevice&&) = delete;

  /// @return What the compute library's @ref Device::adopt takes: its queue
  ///         and family, the queue's mutex, and what was enabled, read back
  ///         from creation rather than restated.
  AdoptedDevice compute_payload() const;
  /// @return What the renderer's @ref Device::adopt takes: its queue, which
  ///         also presents when a surface was made, and the rest as
  ///         @ref compute_payload.
  AdoptedDevice graphics_payload() const;

  /// @brief Block until every queue handed out is idle, holding each queue's
  ///        mutex, so a concurrent submit is excluded rather than raced. Safe
  ///        with both adopters alive.
  void wait_idle() const noexcept;

  /// @brief Give up the surface to the caller, who then destroys it (gfx
  ///        adopts and destroys the surface it presents to); this stops
  ///        tracking it.
  /// @return The surface, or `VK_NULL_HANDLE` if none was made or it was
  ///         released already.
  VkSurfaceKHR release_surface() noexcept;

  /// @return The plan the queues were carved by.
  QueuePlan plan() const noexcept { return plan_; }
  /// @return The renderer's queue family. A buffer both libraries touch names
  ///         both families (@ref BufferDesc::queue_families); duplicates count
  ///         once, so passing both unconditionally is right on every plan.
  std::uint32_t graphics_family() const noexcept { return graphics_family_; }
  /// @return The compute library's queue family.
  std::uint32_t compute_family() const noexcept { return compute_family_; }
  /// @return The instance.
  const Instance& instance() const noexcept { return instance_; }
  /// @return The physical device and its capabilities.
  const PhysicalDeviceInfo& physical() const noexcept { return physical_; }
  /// @return The device.
  VkDevice device() const noexcept { return device_; }
  /// @return The surface, or `VK_NULL_HANDLE`.
  VkSurfaceKHR surface() const noexcept { return surface_; }
  /// @return One line naming the device and the plan taken, for a log or a
  ///         read-out: a device that lands on @ref QueuePlan::SharedQueue
  ///         says so.
  std::string summary() const;

 private:
  explicit SharedDevice(Instance instance);
  AdoptedDevice payload(std::uint32_t family, VkQueue queue,
                        std::mutex* mutex) const;
  std::mutex* compute_mutex() const noexcept;

  Instance instance_;
  PhysicalDeviceInfo physical_;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  QueuePlan plan_ = QueuePlan::SharedQueue;
  std::uint32_t graphics_family_ = 0;
  std::uint32_t compute_family_ = 0;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkQueue compute_queue_ = VK_NULL_HANDLE;
  bool presents_ = false;
  // Guards the graphics queue, and the compute queue too under SharedQueue.
  mutable std::mutex graphics_mutex_;
  // Guards the compute queue under the other plans.
  mutable std::mutex compute_mutex_;
  // What vkCreateDevice was given, which the payloads declare.
  std::vector<std::string> extension_storage_;
  std::vector<const char*> extensions_;
  VkPhysicalDeviceFeatures enabled_features_{};
  bool enabled_timeline_semaphore_ = false;
  bool enabled_scalar_block_layout_ = false;
  bool enabled_dynamic_rendering_ = false;
};

}  // namespace volumetric_kit::core
