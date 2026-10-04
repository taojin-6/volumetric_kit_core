// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file device.hpp
/// @brief The logical device: its queue (and present queue), thread-safe
///        submission, and the create-or-adopt seam that lets several
///        libraries share one `VkDevice`.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Instance;

/// @brief Cast any Vulkan handle to the `std::uint64_t`
///        @ref Device::set_object_name takes.
///
/// One cast cannot serve both word sizes: a non-dispatchable handle is a
/// pointer on 64-bit targets and a bare `uint64_t` on 32-bit ones (a 32-bit
/// Android build among the family's targets), so `reinterpret_cast` is
/// required on one and ill-formed on the other. `VK_NULL_HANDLE` needs its own
/// branch: where handles are pointers it is `nullptr`, which is not a pointer
/// type to `std::is_pointer_v`.
/// @param handle  Any Vulkan handle, or `VK_NULL_HANDLE`.
/// @return The handle as `std::uint64_t`; `0` for `VK_NULL_HANDLE`.
template <typename Handle>
inline std::uint64_t debug_object_handle(Handle handle) noexcept {
  if constexpr (std::is_null_pointer_v<Handle>) {
    (void)handle;
    return 0;
  } else if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<std::uint64_t>(handle);
  } else {
    return static_cast<std::uint64_t>(handle);
  }
}

/// @brief A `VkDevice` someone else created, and what they enabled on it,
///        handed to @ref Device::adopt.
///
/// Vulkan cannot be asked which extensions or features a logical device
/// enabled, so the creator declares them here and @ref Device::adopt checks
/// the declaration against the library's requirements. The extension array
/// need only outlive the `adopt` call; the handles and mutexes must outlive
/// the returned @ref Device.
///
/// @code
/// AdoptedDevice handoff;
/// handoff.instance = instance;
/// handoff.instance_api_version = app_info.apiVersion;
/// handoff.physical_device = physical;
/// handoff.device = device;
/// handoff.queue_family = compute_family;
/// handoff.queue = compute_queue;
/// handoff.submit_mutex = &compute_queue_mutex;  // shared with another library
/// handoff.enabled_timeline_semaphore = true;
/// VKC_ASSIGN(Device borrowed, Device::adopt(handoff, reqs));
/// @endcode
struct AdoptedDevice {
  /// The instance `device` belongs to.
  VkInstance instance = VK_NULL_HANDLE;
  /// The Vulkan version `instance` was created with
  /// (`VkApplicationInfo::apiVersion`). It bounds what the device may use,
  /// whatever the physical device reports, and what @ref Device::adopt may
  /// query. Required: the default, `0`, fails @ref Device::adopt.
  std::uint32_t instance_api_version = 0;
  /// The physical device `device` was created on.
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  /// The logical device to borrow; @ref Device never destroys it.
  VkDevice device = VK_NULL_HANDLE;
  /// The queue assigned to this library, and its family.
  std::uint32_t queue_family = 0;
  /// See @ref queue_family.
  VkQueue queue = VK_NULL_HANDLE;
  /// When the queue is shared with another library, the mutex every
  /// operation on it must hold (Vulkan requires a queue be externally
  /// synchronized); null when it is this library's alone.
  std::mutex* submit_mutex = nullptr;
  /// Whether a present queue is assigned too.
  bool has_present = false;
  /// The present queue and its family; the queue itself when they are one.
  std::uint32_t present_family = 0;
  /// See @ref present_family.
  VkQueue present_queue = VK_NULL_HANDLE;
  /// When the present queue is a different queue shared with another
  /// library, the mutex it must hold; ignored when it is @ref queue.
  std::mutex* present_mutex = nullptr;
  /// The device extensions the creator enabled.
  const char* const* enabled_extensions = nullptr;
  /// The length of @ref enabled_extensions.
  std::uint32_t enabled_extension_count = 0;
  /// The core (1.0) features the creator enabled.
  VkPhysicalDeviceFeatures enabled_features{};
  /// Whether the creator enabled `timelineSemaphore`. Each of these flags
  /// defaults to `false`, so a creator that declares nothing fails
  /// @ref Device::adopt loudly: a device that *supports* a feature has not
  /// thereby enabled it, and using an unenabled feature is invalid usage the
  /// driver need not report.
  bool enabled_timeline_semaphore = false;
  /// Whether the creator enabled `scalarBlockLayout`.
  bool enabled_scalar_block_layout = false;
  /// Whether the creator enabled `dynamicRendering`.
  bool enabled_dynamic_rendering = false;
  /// Whether the creator's *instance* enabled `VK_EXT_debug_utils`. Optional:
  /// `false` costs only the profiler labels.
  bool enabled_debug_utils = false;
};

/// @brief Owns, or borrows, a `VkDevice`, its queue and optional present
///        queue, and the command pools it submits from.
///
/// The one device type of the family: recon, gfx and calib each run on one,
/// created or adopted. A device made by @ref create owns its `VkDevice`; one
/// from @ref adopt borrows it and destroys only what it made itself.
///
/// Submission is thread-safe: each submit records on a command pool no other
/// submit is using, and only the `vkQueueSubmit` is serialized, under
/// @ref submit_mutex. The fence each submit waits on is kept for the next, as
/// creating one per submit cost an RTX 5090 about 0.3 ms.
///
/// @warning The instance, and an adopted device's handles and mutexes, must
///          outlive this object; it stores only borrowed handles.
///
/// @code
/// VKC_ASSIGN(Instance instance, Instance::create({}));
/// VKC_ASSIGN(PhysicalDeviceInfo gpu, instance.select_physical_device(reqs));
/// VKC_ASSIGN(Device device, Device::create(instance, gpu, reqs));
/// VKC_TRY(device.submit_single_time([&](VkCommandBuffer cmd) {
///   vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, 0);
/// }));
/// @endcode
class VKC_VULKAN_API Device {
 public:
  /// @brief Create and own a logical device on @p physical that meets
  ///        @p reqs.
  ///
  /// Enables @p reqs' extensions, features and feature chain; each of its
  /// optional extensions the device offers; `VK_KHR_portability_subset`
  /// where the device exposes it (the spec requires it); and
  /// `VK_KHR_swapchain` with presentation. Creates one queue on the first
  /// family with every @ref DeviceRequirements::queue_flags bit, and a present
  /// queue, which is that same queue when its family can present.
  /// @param instance  The instance @p physical belongs to; it must outlive the
  ///                  device. Its debug utils decide whether the label entry
  ///                  points resolve.
  /// @param physical  The physical device's capabilities, captured on
  ///                  @p instance: as @ref Instance::select_physical_device
  ///                  returns them, or from @ref PhysicalDeviceInfo::query
  ///                  with `instance.api_version()`. Their usable version must
  ///                  reach @ref DeviceRequirements::api_version.
  /// @param reqs      What the device must provide.
  /// @param surface   The surface to present to; required when
  ///                  @ref DeviceRequirements::needs_present.
  /// @return The device; @ref Status::Code::InvalidArgument for an empty
  ///         @p physical, one captured on a higher-version instance, or a
  ///         missing @p surface; @ref Status::Code::Unsupported naming the
  ///         first requirement @p physical fails; or a backend @ref Status.
  static Result<Device> create(const Instance& instance,
                               const PhysicalDeviceInfo& physical,
                               const DeviceRequirements& reqs,
                               VkSurfaceKHR surface = VK_NULL_HANDLE);

  /// @brief @ref create on an instance made by someone else.
  /// @param instance  The instance @p physical belongs to; it must outlive the
  ///                  device.
  /// @param instance_debug_utils_enabled  Whether @p instance enabled
  ///                  `VK_EXT_debug_utils`. Vulkan cannot be asked, and an
  ///                  entry point of an extension not enabled is not portable
  ///                  (a directly linked MoltenVK returns a live pointer that
  ///                  must not be called), so the caller declares it.
  /// @param physical  The physical device's capabilities, from
  ///                  @ref PhysicalDeviceInfo::query with the version
  ///                  @p instance was created with, which bounds what the
  ///                  device may use.
  /// @param reqs      What the device must provide.
  /// @param surface   The surface to present to, as above.
  /// @return The device; @ref Status::Code::InvalidArgument for an empty
  ///         @p physical or a missing @p surface; otherwise as the overload
  ///         above.
  static Result<Device> create(VkInstance instance,
                               bool instance_debug_utils_enabled,
                               const PhysicalDeviceInfo& physical,
                               const DeviceRequirements& reqs,
                               VkSurfaceKHR surface = VK_NULL_HANDLE);

  /// @brief Borrow a `VkDevice` someone else created, checking that it meets
  ///        @p reqs.
  ///
  /// Every requirement must be both supported by the physical device and
  /// declared enabled in @p adopted. The returned device never destroys the
  /// `VkDevice`; it makes and destroys only its own command pools.
  /// @param adopted  The handles, the queues assigned to this library, and
  ///                 what the creator enabled.
  /// @param reqs     What this library needs.
  /// @return The borrowing device; @ref Status::Code::InvalidArgument for a
  ///         null handle, an unset instance version, a queue family out of
  ///         range, or presentation without a present queue; or
  ///         @ref Status::Code::Unsupported naming the first requirement not
  ///         supported (within the instance's version) or not declared
  ///         enabled.
  static Result<Device> adopt(const AdoptedDevice& adopted,
                              const DeviceRequirements& reqs);

  ~Device();
  Device(Device&& other) noexcept;
  Device& operator=(Device&& other) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  /// @return The logical device (`VK_NULL_HANDLE` when moved-from).
  VkDevice handle() const noexcept { return state_.device; }
  /// @return The physical device.
  VkPhysicalDevice physical_device() const noexcept { return state_.physical; }
  /// @return Whether this object owns, and will destroy, the `VkDevice`:
  ///         `false` for one from @ref adopt.
  bool owns_device() const noexcept { return state_.owns_device; }
  /// @return The physical device's capabilities, captured at create or adopt;
  ///         `caps().api_version()` is the version the device may use.
  const PhysicalDeviceInfo& caps() const noexcept { return caps_; }

  /// @return The queue this library submits to.
  VkQueue queue() const noexcept { return state_.queue; }
  /// @return Its family.
  std::uint32_t queue_family() const noexcept { return state_.queue_family; }
  /// @brief The capabilities @ref queue_family advertises (`VK_QUEUE_*`).
  ///
  /// The family may be compute-only, e.g. a dedicated async-compute family
  /// an embedder assigned. That matters to a pipeline barrier, which may name
  /// only stages its queue family supports: the vertex-input stage needs a
  /// graphics family. Read from the driver, never assumed.
  /// @return The family's `queueFlags`.
  VkQueueFlags queue_flags() const noexcept { return state_.queue_flags; }
  /// @return The family's `timestampValidBits`, `0` when the queue writes no
  ///         timestamps (MoltenVK may say so). Gate GPU timing on a non-zero
  ///         value; convert ticks to nanoseconds with
  ///         `caps().limits().timestampPeriod`.
  std::uint32_t timestamp_valid_bits() const noexcept {
    return state_.timestamp_valid_bits;
  }

  /// @return Whether a present queue exists.
  bool has_present() const noexcept {
    return state_.present_queue != VK_NULL_HANDLE;
  }
  /// @return The present queue, which is @ref queue when one family serves
  ///         both; valid only when @ref has_present.
  VkQueue present_queue() const noexcept { return state_.present_queue; }
  /// @return Its family; valid only when @ref has_present.
  std::uint32_t present_family() const noexcept {
    return state_.present_family;
  }

  /// @param name  A device extension name.
  /// @return Whether @p name is enabled on the device: for a created device,
  ///         each required, optional-and-offered, or implied extension; for an
  ///         adopted one, what its creator declared.
  bool extension_enabled(const char* name) const;

  /// @return The mutex every operation on @ref queue holds: the embedder's on
  ///         a queue shared with another library, else this device's own.
  ///         Never null. Hold it around a `vkQueueSubmit` or
  ///         `vkQueueWaitIdle` of your own; the member functions take it for
  ///         you.
  /// @warning A device's own mutex does not move with it: a moved-to device
  ///          locks its own, so do not keep this pointer across a move.
  std::mutex* submit_mutex() const noexcept {
    return state_.submit_mutex != nullptr ? state_.submit_mutex : &queue_mutex_;
  }

  /// @brief Submit to @ref queue, holding @ref submit_mutex.
  /// @param count    The number of `VkSubmitInfo`s in @p submits.
  /// @param submits  The batch.
  /// @param fence    Signalled on completion; may be `VK_NULL_HANDLE`.
  /// @return The `vkQueueSubmit` result.
  VkResult queue_submit(std::uint32_t count, const VkSubmitInfo* submits,
                        VkFence fence) const;

  /// @brief Record a one-time command buffer, submit it, and wait for the
  ///        GPU to finish: the simplest dispatch.
  ///
  /// Takes a command buffer on a pool of this call's own, begins it
  /// (`ONE_TIME_SUBMIT`), lets @p record fill it, ends and submits it, and
  /// waits on a kept fence. Safe from several threads at once, and @p record
  /// may itself submit on this device; what it records -- descriptor sets,
  /// buffers -- must still be its own. Blocking, so it suits setup and
  /// single-shot work; per-frame work batches its own submits.
  /// @param record     Records into the command buffer it is given.
  /// @param in_flight  Optional; set to `true` when the call failed after the
  ///                   device had the work -- a failed wait, or a submit that
  ///                   lost the device -- so the device may still run it, and
  ///                   whatever it records must stay alive; `false` otherwise.
  ///                   A @ref CommandBatch keeps its staging alive by it.
  /// @return OK once the work completes; or the failed step's backend
  ///         @ref Status. A failed wait leaves the command buffer and fence
  ///         to a device that may still run them, until the device is
  ///         destroyed; if the work is still unfinished then, a `VkDevice`
  ///         this object owns is leaked with them rather than destroyed
  ///         under running work (logged as an error).
  ///
  /// TODO: add the overload that brackets the work with a GPU timestamp span
  /// (recon's GpuTimer) with the tier's timers.
  Status submit_single_time(const std::function<void(VkCommandBuffer)>& record,
                            bool* in_flight = nullptr) const;

  /// @brief Submit an already-recorded, ended command buffer to @ref queue
  ///        and wait for it, on a kept fence.
  /// @param cmd  A command buffer in the executable state, allocated from a
  ///             pool of @ref queue_family; it stays the caller's.
  /// @return OK once it completes; or a backend @ref Status. After a failed
  ///         wait the device may still run @p cmd, so whatever it uses must
  ///         stay alive, and the device is then destroyed as
  ///         @ref submit_single_time says.
  Status submit_and_wait(VkCommandBuffer cmd) const;

  /// @brief Present on @ref present_queue, holding its mutex.
  /// @param present_info  The present descriptor.
  /// @return The `vkQueuePresentKHR` result; `VK_SUBOPTIMAL_KHR` and
  ///         `VK_ERROR_OUT_OF_DATE_KHR` come back so the caller can recreate
  ///         its swapchain.
  /// @pre @ref has_present.
  VkResult queue_present(const VkPresentInfoKHR& present_info) const;

  /// @brief Wait until this device's own queues are idle, holding their
  ///        mutexes.
  ///
  /// The shared-device-safe `vkDeviceWaitIdle`: that call idles every queue,
  /// another library's included, and cannot be synchronized against their
  /// mutexes.
  /// @return OK once the queues drain; or a backend @ref Status (device lost).
  Status wait_idle() const;

  /// @return Whether the `VK_EXT_debug_utils` entry points resolved, so
  ///         @ref set_object_name and the labels reach a profiler. Every such
  ///         call is a well-defined no-op otherwise.
  bool debug_labels_available() const noexcept {
    return state_.set_object_name != nullptr;
  }
  /// @brief Name a Vulkan object, so a GPU capture shows the name instead of
  ///        a raw handle. A no-op without @ref debug_labels_available.
  /// @param type    The object's `VkObjectType`.
  /// @param handle  The object, as @ref debug_object_handle casts it.
  /// @param name    The name, copied by the driver; ignored when null.
  void set_object_name(VkObjectType type, std::uint64_t handle,
                       const char* name) const noexcept;
  /// @brief Open a named region in @p cmd; @ref end_debug_label closes it.
  ///        Nsight shows it as a range, and MoltenVK as a Metal debug group.
  ///        A no-op without @ref debug_labels_available.
  /// @param cmd   A recording command buffer.
  /// @param name  The region's name; null opens nothing.
  void begin_debug_label(VkCommandBuffer cmd, const char* name) const noexcept;
  /// @brief Close the region @ref begin_debug_label opened on @p cmd.
  ///
  /// Takes the same @p name so the pair skips on identical conditions: a null
  /// name opened nothing, and closing anyway would pop a region never pushed.
  /// @param cmd   The command buffer the region is open on.
  /// @param name  The name passed to @ref begin_debug_label.
  void end_debug_label(VkCommandBuffer cmd, const char* name) const noexcept;

  /// @return Whether the device exports memory as a file descriptor
  ///         (`VK_KHR_external_memory_fd`), as @ref memory_fd needs.
  bool exports_memory() const noexcept {
    return state_.get_memory_fd != nullptr;
  }
  /// @brief `vkGetMemoryFdKHR`: an opaque file descriptor for @p memory,
  ///        which the caller then owns.
  /// @param memory  Memory allocated exportable as an opaque fd.
  /// @param fd      Receives the descriptor.
  /// @return `VK_ERROR_EXTENSION_NOT_PRESENT` without @ref exports_memory;
  ///         otherwise the call's result.
  VkResult memory_fd(VkDeviceMemory memory, int* fd) const noexcept;
  /// @return Whether the device imports Metal objects as Vulkan ones
  ///         (`VK_EXT_metal_objects`, MoltenVK), so a VideoToolbox picture can
  ///         stay on the GPU.
  bool imports_metal_textures() const noexcept { return state_.metal_objects; }

 private:
  // Everything trivially copyable a device holds, so a move copies it and
  // resets the source to `State{}` in one assignment, with no member list to
  // keep in step.
  struct State {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    bool owns_device = true;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkQueueFlags queue_flags = 0;
    std::uint32_t timestamp_valid_bits = 0;
    VkQueue present_queue = VK_NULL_HANDLE;
    std::uint32_t present_family = 0;
    // The embedder's, when a queue is shared with another library; else
    // null, and the device's own mutex guards it.
    std::mutex* submit_mutex = nullptr;
    std::mutex* present_mutex = nullptr;
    // Resolved once, all three or none; null when the instance did not
    // enable VK_EXT_debug_utils.
    PFN_vkSetDebugUtilsObjectNameEXT set_object_name = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_label = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT end_label = nullptr;
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
    bool metal_objects = false;
  };

  // The fence a submit signals and, made the first time a submit records
  // into it, a command buffer on a pool of its own (submit_and_wait records
  // nothing). A pool must be externally synchronized, so each submit takes
  // one no other holds: a free one, or a new one when all are in use. It
  // comes back once its wait is done; `made_` keeps every one, so destroy()
  // frees them all, and one left to the device after a failed wait is
  // `pending` there and waited for first. The fence names it.
  struct Command {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    bool pending = false;
  };

  Device() = default;
  void destroy() noexcept;
  // Resolves the debug-utils entry points (on the creator's word that the
  // instance enabled the extension), the memory-fd export and the
  // metal-objects flag, from enabled_extensions_.
  void resolve_entry_points(bool instance_debug_utils) noexcept;
  std::mutex* present_mutex() const noexcept;
  // `record`: the submit records, so the command needs its buffer.
  Result<Command> take_command(bool record) const;
  // Gives `command` its pool and buffer, in made_ too; called holding
  // commands_mutex_.
  Status add_command_buffer(Command* command) const;
  void give_back(const Command& command) const noexcept;
  void leave_to_device(const Command& command) const noexcept;
  // Submits `cmd` signalling `command.fence`, waits, and resets the fence.
  // `*reusable` says whether `command` may be given back: not after a failed
  // wait (it is left to the device) or a failed fence reset. `*in_flight`
  // says whether the device may still run `cmd`: only after a failed wait.
  Status submit_waiting(VkCommandBuffer cmd, const Command& command,
                        bool* reusable, bool* in_flight) const;

  State state_;
  PhysicalDeviceInfo caps_;
  std::vector<std::string> enabled_extensions_;
  // Not moved: a moved-to device uses its own (see submit_mutex()).
  mutable std::mutex queue_mutex_;
  mutable std::mutex present_queue_mutex_;
  mutable std::mutex commands_mutex_;  // guards made_ and free_commands_
  mutable std::vector<Command> made_;
  mutable std::vector<Command> free_commands_;
};

}  // namespace volumetric_kit::core
