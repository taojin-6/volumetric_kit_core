// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file device.hpp
/// @brief The logical device: its queue (and present queue), thread-safe
///        submission, and the create-or-adopt seam that lets several
///        libraries share one `VkDevice`.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class GpuStageScope;
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

/// @brief What a logical device enabled of the features
///        @ref DeviceRequirements names: its core features and the timeline,
///        scalar and dynamic-rendering flags.
///
/// A @ref Device keeps one for @ref Device::check_enabled: what
/// @ref Device::create enabled, or what an @ref AdoptedDevice declares.
/// Every member defaults to off, so a creator that declares nothing fails
/// @ref Device::adopt loudly: a device that *supports* a feature has not
/// thereby enabled it, and using a feature that was not enabled is invalid
/// usage the driver need not report.
///
/// @code
/// EnabledFeatures enabled;
/// enabled.core.shaderInt64 = VK_TRUE;  // as VkDeviceCreateInfo enabled them
/// enabled.timeline_semaphore = true;
/// handoff.enabled_features = enabled;
/// @endcode
struct EnabledFeatures {
  /// The core (1.0) features.
  VkPhysicalDeviceFeatures core{};
  /// `timelineSemaphore`.
  bool timeline_semaphore = false;
  /// `scalarBlockLayout`.
  bool scalar_block_layout = false;
  /// `dynamicRendering`.
  bool dynamic_rendering = false;
};

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
/// handoff.enabled_features.timeline_semaphore = true;
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
  /// The device extensions the creator enabled. With
  /// `VK_EXT_memory_budget` among them, an @ref Allocator allocates within
  /// the driver's own heap budgets.
  const char* const* enabled_extensions = nullptr;
  /// The length of @ref enabled_extensions.
  std::uint32_t enabled_extension_count = 0;
  /// The features the creator enabled, each off by default. The device
  /// keeps them, so a later @ref Device::check_enabled holds a library's
  /// requirements to this declaration too.
  EnabledFeatures enabled_features;
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
/// avoiding the driver overhead of creating one per submit.
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
  /// where the device exposes it (the spec requires it);
  /// `VK_EXT_memory_budget` where offered, so an @ref Allocator allocates
  /// within the driver's own heap budgets; and `VK_KHR_swapchain` with
  /// presentation. Creates one queue on the first family with every
  /// @ref DeviceRequirements::queue_flags bit, and a present queue, which is
  /// that same queue when its family can present.
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
  ///         timestamps, as some implementations report. Gate GPU timing on a
  ///         non-zero value; convert ticks to nanoseconds with
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

  /// @brief Whether the device enabled everything @p reqs requires.
  ///
  /// For a library handed a device it did not make: a device created for
  /// another library's requirements, or with the defaults, may lack a
  /// feature this one's shaders use, and using a feature that was not
  /// enabled is invalid usage a driver need not report. Holds @p reqs to the
  /// queue's capabilities and a present queue; to what @ref create and
  /// @ref adopt hold them to -- the usable API version, and the physical
  /// device's support for each required extension and feature, within the
  /// version that makes it core; and to the device's record of what it
  /// enabled (@ref EnabledFeatures): for a created device, its requirements
  /// and any of the three flags its feature chain set; for an adopted one,
  /// what its creator declared. Optional extensions are not checked.
  ///
  /// A device keeps no record of a feature chain, so requirements that carry
  /// one are refused rather than passed unchecked: check those features by
  /// other means, and pass the requirements without the chain, with any of
  /// the three flags it set moved to their fields.
  ///
  /// @code
  /// // Before building a kernel on a device someone else made.
  /// VKC_TRY(device.check_enabled(reqs).with_context("Kernels::create"));
  /// @endcode
  /// @param reqs  The requirements.
  /// @return OK; @ref Status::Code::Unsupported naming the first requirement
  ///         the device does not meet; or @ref Status::Code::InvalidArgument
  ///         for a moved-from device, or @p reqs with a
  ///         @ref DeviceRequirements::feature_chain.
  Status check_enabled(const DeviceRequirements& reqs) const;

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
  /// @param record      Records into the command buffer it is given.
  /// @param keep_alive  Optional; what the work reads or writes that the
  ///                    caller frees once this returns -- a
  ///                    @ref CommandBatch's staging. Released before this
  ///                    returns, unless the call fails after the device had
  ///                    the work (a failed wait, or a submit that lost the
  ///                    device): the device may still run it then, so it
  ///                    keeps this with the command buffer, as below.
  /// @param in_flight   Optional; set to whether the call failed after the
  ///                    device had the work, which may still run: a failed
  ///                    wait, or a submit that lost the device. False on
  ///                    success, and on a failure before the device had it
  ///                    (a begin, end or submit it refused), which ran none
  ///                    of it.
  /// @return OK once the work completes; or the failed step's backend
  ///         @ref Status. A failed wait leaves the command buffer, fence and
  ///         @p keep_alive to a device that may still run them, until the
  ///         device is destroyed: it waits for the work then, and frees them
  ///         before the `VkDevice`. If the work is still unfinished then, a
  ///         `VkDevice` this object owns is leaked with them rather than
  ///         destroyed under running work (logged as an error).
  Status submit_single_time(const std::function<void(VkCommandBuffer)>& record,
                            std::shared_ptr<void> keep_alive = nullptr,
                            bool* in_flight = nullptr) const;

  /// @brief @ref submit_single_time, with the recorded work inside a device
  ///        span of @p stage, resolved once the fence has signalled.
  ///
  /// The span covers what @p record records, not the command buffer's
  /// allocation, the submit or the wait -- the difference a wall-clock row
  /// cannot show. An inert @p stage (null metrics) is exactly the untimed
  /// call. The span settles as @ref GpuTimer::settle says: resolved on
  /// success, a failed read logged rather than returned; dropped when the
  /// work never ran, a @p record that throws included; and the timer retired
  /// when the device may still run it. The timer's query pool goes with
  /// @p keep_alive, so a timer destroyed after that frees no queries the work
  /// may still write. @p record may itself submit, timed on @p stage too:
  /// each submit reads only its own span.
  ///
  /// @code
  /// GpuStageScope stage(metrics, timer, "rebuild");
  /// VKC_TRY(device.submit_single_time(record, stage));
  /// @endcode
  /// @param record      Records into the command buffer it is given.
  /// @param stage       The span's timer and label.
  /// @param keep_alive  As the untimed overload.
  /// @return As the untimed overload.
  Status submit_single_time(const std::function<void(VkCommandBuffer)>& record,
                            GpuStageScope& stage,
                            std::shared_ptr<void> keep_alive = nullptr) const;

  class PendingSubmit;

  /// @brief Record a one-time command buffer and submit it after every value
  ///        in @p wait, setting every value in @p signal once it completes,
  ///        without waiting for it: @ref submit_single_time, with the wait
  ///        left to the @ref PendingSubmit it returns.
  ///
  /// Nothing the buffer records starts before every value in @p wait is
  /// reached: the wait blocks all commands (`ALL_COMMANDS`), which is valid on
  /// any queue, whatever its capabilities. The values the submissions that
  /// reach them set make their writes available, and the wait makes them
  /// visible to this one, on this queue or another; across queue families,
  /// an `EXCLUSIVE` buffer still needs its ownership transfer. A value in
  /// @p signal is set only once everything recorded has completed, its writes
  /// available to whatever waits for it. A wait may precede the submission
  /// that will reach its value, but the host must then make sure one does --
  /// or signal it (@ref TimelineSemaphore::signal) -- or the work never runs.
  ///
  /// What the work uses -- buffers, images, pipelines, descriptor sets, and
  /// the semaphores named -- must stay alive and unchanged until it
  /// completes. With @p wait and @p signal empty it needs no timeline
  /// semaphores, and is @ref submit_single_time without the wait.
  ///
  /// @code
  /// VKC_ASSIGN(Device::PendingSubmit pending,
  ///            device.submit_pending(record, {{&uploaded, n}}, {{&done, n}}));
  /// // ... other host work, while the device runs it ...
  /// VKC_TRY(pending.wait());
  /// @endcode
  /// @param record      Records into the command buffer it is given.
  /// @param wait        Values to reach before anything recorded starts.
  /// @param signal      Values to set once everything recorded completes; each
  ///                    above its semaphore's current value.
  /// @param keep_alive  Optional; what the work uses that the caller would
  ///                    otherwise free: held by the @ref PendingSubmit until
  ///                    a wait sees the work complete, or by the device if
  ///                    the wait fails, as @ref submit_single_time says.
  /// @return The submission; @ref Status::Code::InvalidArgument for a
  ///         moved-from device, a null or empty semaphore, one made on
  ///         another `VkDevice`, or a value to set that is not above its
  ///         semaphore's current one; @ref Status::Code::Unsupported for a
  ///         timeline value on a device that did not enable
  ///         `timelineSemaphore`; or the failed step's backend @ref Status.
  ///         All refusals come before the command buffer is taken. A submit
  ///         that loses the device leaves the work to it, as a failed wait
  ///         does.
  Result<PendingSubmit> submit_pending(
      const std::function<void(VkCommandBuffer)>& record,
      const std::vector<TimelinePoint>& wait,
      const std::vector<TimelinePoint>& signal,
      std::shared_ptr<void> keep_alive = nullptr) const;

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
    // What create enabled, or adopt's creator declared, for check_enabled.
    EnabledFeatures enabled;
  };

  // The fence a submit signals and, made the first time a submit records
  // into it, a command buffer on a pool of its own (submit_and_wait records
  // nothing). A pool must be externally synchronized, so each submit takes
  // one no other holds: a free one, or a new one when all are in use. It
  // comes back once its wait is done -- a PendingSubmit's, for
  // submit_pending -- and `made_` keeps every one, so destroy()
  // frees them all, and one left to the device after a failed wait is
  // `pending` there and waited for first, with what its work uses kept in
  // `keep_alive` until then. The fence names it.
  struct Command {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    bool pending = false;
    std::shared_ptr<void> keep_alive;
  };

  Device() = default;
  void destroy() noexcept;
  // The record half of check_enabled, which adopt runs on the declaration:
  // whether the device enabled each of `required` (reqs'
  // required_extensions) and each feature `reqs` names. A refusal names the
  // AdoptedDevice field to fix when the record is an adopted device's
  // declaration.
  Status check_record(const DeviceRequirements& reqs,
                      const std::vector<std::string>& required) const;
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
  void leave_to_device(const Command& command,
                       std::shared_ptr<void> keep_alive) const noexcept;
  // Submits `cmd` signalling `command.fence`, waits, and resets the fence.
  // `*reusable` says whether `command` may be given back: not after a failed
  // wait, which leaves it, and `keep_alive` with it, to the device; nor after
  // a failed fence reset.
  Status submit_waiting(VkCommandBuffer cmd, const Command& command,
                        std::shared_ptr<void> keep_alive, bool* reusable) const;

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

/// @brief Work a @ref Device has and the host has not yet waited on: what
///        @ref Device::submit_pending returns.
///
/// Holds the command buffer and fence the work runs on, and its
/// `keep_alive`, until a @ref wait sees it complete: then the command buffer
/// and fence go back to the device for the next submit, and the `keep_alive`
/// is released. A wait that fails leaves them to the device, which may still
/// run the work, as a failed wait of @ref Device::submit_single_time does.
/// Destroying an unfinished one waits for it first, so what the work uses is
/// never freed under it; an empty one (default, or moved from) has nothing
/// to wait for.
///
/// It may be waited on from a thread other than the one that submitted it,
/// but not from two at once.
///
/// @warning The @ref Device must outlive it, and must not move meanwhile.
///
/// @code
/// VKC_ASSIGN(Device::PendingSubmit pending,
///            device.submit_pending(record, {}, {{&done, frame}}));
/// if (pending.ready()) VKC_TRY(pending.wait());  // returns at once
/// @endcode
class VKC_VULKAN_API Device::PendingSubmit {
 public:
  /// @brief Construct an empty submission: @ref ready, nothing to wait for.
  PendingSubmit() noexcept = default;
  /// Waits for the work if it is unfinished.
  ~PendingSubmit();
  PendingSubmit(const PendingSubmit&) = delete;
  PendingSubmit& operator=(const PendingSubmit&) = delete;
  PendingSubmit(PendingSubmit&& other) noexcept;
  /// Waits for this one's own unfinished work before taking @p other's.
  PendingSubmit& operator=(PendingSubmit&& other) noexcept;

  /// @return Whether @ref wait would return without blocking: the work has
  ///         completed, failed, or the submission is empty. Never blocks.
  bool ready() const;

  /// @brief Wait until the work completes, or the timeout passes.
  /// @param timeout_ns  The longest wait, in nanoseconds.
  /// @return OK once it has completed, and on every later call, as on an
  ///         empty submission; a backend @ref Status carrying `VK_TIMEOUT`
  ///         when the timeout passes first, the work still pending, for a
  ///         later wait; or the failure (device lost), after which the work is
  ///         the device's, and every later call returns the same failure.
  Status wait(std::uint64_t timeout_ns = UINT64_MAX);

  /// @return Whether the device may still run the work: until a wait sees it
  ///         complete, and for good after one that failed.
  bool in_flight() const noexcept { return running_ || !failed_.ok(); }

 private:
  friend class Device;

  // Waits for unfinished work, logging a failure: the destructor's and a
  // move assignment's, which cannot return one.
  void finish() noexcept;

  const Device* device_ = nullptr;
  Command command_;
  std::shared_ptr<void> keep_alive_;
  bool running_ = false;  // submitted; no wait has seen it complete or fail
  Status failed_;         // a failed wait's status, returned again
};

}  // namespace volumetric_kit::core
