// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file device_requirements.hpp
/// @brief What a library needs from a `VkDevice`, how two libraries' needs
///        combine, and whether a physical device meets them.
///
/// Each library states its needs as a @ref DeviceRequirements -- recon a
/// compute queue with timeline semaphores and scalar block layout, gfx a
/// graphics queue on Vulkan 1.3 with dynamic rendering and, for a window,
/// presentation. The same struct drives @ref Instance::select_physical_device,
/// @ref Device::create and @ref Device::adopt, and @ref merge combines two
/// libraries' into the one a shared device must satisfy, so all of them agree
/// on what "supported" means.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief What a library needs from a logical device.
///
/// Raw Vulkan data and owned strings only, so the struct carries no type a
/// sibling library must import, and no pointer that dangles once the caller's
/// names go out of scope -- except @ref feature_chain, which is the caller's.
/// The defaults are the floor every library shares: Vulkan 1.2, a compute
/// queue, timeline semaphores.
///
/// @code
/// DeviceRequirements renderer;
/// renderer.api_version = VK_API_VERSION_1_3;
/// renderer.queue_flags = VK_QUEUE_GRAPHICS_BIT;
/// renderer.needs_present = true;
/// renderer.dynamic_rendering = true;
///
/// DeviceRequirements fusion;
/// fusion.scalar_block_layout = true;
///
/// Result<DeviceRequirements> shared = merge(renderer, fusion);
/// @endcode
///
/// TODO: give it the instance-level needs (`VK_EXT_debug_utils` for labels, a
/// window's surface extensions) with the shared-device bootstrap (V5), the
/// first code to create an instance from requirements.
struct DeviceRequirements {
  /// The lowest Vulkan version usable on the device: the lower of what it
  /// reports and what its instance negotiated
  /// (@ref PhysicalDeviceInfo::api_version).
  std::uint32_t api_version = VK_API_VERSION_1_2;
  /// The capabilities the device's queue must all have. A compute or graphics
  /// queue supports transfer whether or not its family advertises
  /// `VK_QUEUE_TRANSFER_BIT`, so do not add that bit for transfers: a
  /// conformant compute family may legally omit it.
  VkQueueFlags queue_flags = VK_QUEUE_COMPUTE_BIT;
  /// A queue that can present to the surface passed at creation, and
  /// `VK_KHR_swapchain`, which this implies -- do not list it again.
  bool needs_present = false;
  /// Device extensions the device must support and enable.
  std::vector<std::string> extensions;
  /// Device extensions to enable where the device offers them, and skip
  /// where it does not -- e.g. `VK_KHR_external_memory_fd`, which lets a
  /// hardware decoder write into the library's buffers but is not required.
  /// Ask @ref Device::extension_enabled whether one was.
  std::vector<std::string> optional_extensions;
  /// Core (1.0) features to enable.
  VkPhysicalDeviceFeatures features{};
  /// `timelineSemaphore` (Vulkan 1.2 core).
  bool timeline_semaphore = true;
  /// `scalarBlockLayout` (Vulkan 1.2 core): recon's compute-shader buffer ABI.
  bool scalar_block_layout = false;
  /// `dynamicRendering` (Vulkan 1.3 core): gfx's render targets.
  bool dynamic_rendering = false;
  /// Further feature structs to enable (a caller-owned `pNext` chain of
  /// `*Features` structs, each with its `sType` set), appended to the chain
  /// @ref Device::create builds. Query support first; a feature the device
  /// lacks fails creation.
  ///
  /// @warning Mutable, and written: Vulkan forbids a chain holding both a
  ///          version aggregate and a struct it subsumes
  ///          (VUID-VkDeviceCreateInfo-pNext-02830), so when this chain
  ///          already carries `VkPhysicalDeviceVulkan12Features` /
  ///          `VkPhysicalDeviceVulkan13Features` or one of the standalone
  ///          timeline / scalar / dynamic-rendering structs, @ref
  ///          Device::create sets the bit in the caller's struct instead of
  ///          linking its own. The structs must outlive the create call.
  ///          @ref Device::adopt cannot inspect an opaque chain: the device's
  ///          creator vouches for it.
  void* feature_chain = nullptr;
};

/// @brief The requirements a device shared by two libraries must satisfy.
///
/// The higher API version; the union of the queue capabilities, extensions,
/// core features and feature flags; presentation if either needs it. An
/// extension one side requires and the other merely accepts is required.
/// @param a  One library's requirements.
/// @param b  The other's.
/// @return The combined requirements; @ref Status::Code::InvalidArgument when
///         both carry a different @ref DeviceRequirements::feature_chain,
///         which cannot be merged without writing to the caller's structs --
///         link the two chains into one and set it on one side.
VKC_VULKAN_API Result<DeviceRequirements> merge(const DeviceRequirements& a,
                                                const DeviceRequirements& b);

/// @brief The queue families a supported device would use.
///
/// @code
/// VKC_ASSIGN(const DeviceSupport support,
///            check_device_support(caps, reqs, surface));
/// if (support.present_family &&
///     *support.present_family != support.queue_family) {
///   // ... swapchain images change queue family before each present ...
/// }
/// @endcode
struct DeviceSupport {
  /// The first family whose capabilities include every
  /// @ref DeviceRequirements::queue_flags bit.
  std::uint32_t queue_family = 0;
  /// The family that presents: @ref queue_family itself when it can present
  /// to the surface, else the first family that can; empty without
  /// @ref DeviceRequirements::needs_present.
  std::optional<std::uint32_t> present_family;
};

/// @brief Whether a physical device meets a set of requirements.
///
/// The check device selection and @ref Device::create share: the usable API
/// version, a queue family with the capabilities, a present family for the
/// surface, every required extension (`VK_KHR_swapchain` with presentation),
/// every core feature, and the timeline / scalar / dynamic-rendering
/// features, each within the usable version that makes it core.
/// @ref DeviceRequirements::feature_chain is not inspected.
/// @param caps     The device's captured capabilities.
/// @param reqs     The requirements.
/// @param surface  The surface to present to; required when
///                 @ref DeviceRequirements::needs_present, else ignored.
/// @return The families the device would use; @ref Status::Code::Unsupported
///         naming the first requirement the device fails; or
///         @ref Status::Code::InvalidArgument for an empty @p caps or a
///         missing @p surface.
VKC_VULKAN_API Result<DeviceSupport> check_device_support(
    const PhysicalDeviceInfo& caps, const DeviceRequirements& reqs,
    VkSurfaceKHR surface = VK_NULL_HANDLE);

}  // namespace volumetric_kit::core
