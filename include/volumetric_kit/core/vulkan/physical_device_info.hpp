// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file physical_device_info.hpp
/// @brief Read-only capabilities of one physical device, captured once.

#include <cstdint>
#include <string>
#include <vector>

#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Cached, read-only capabilities of one `VkPhysicalDevice`.
///
/// @ref query captures the properties and limits, the memory heaps and types,
/// the supported device extensions, the queue families, the core (1.0)
/// features, and the three newer features the family's libraries require --
/// timeline semaphores and scalar block layout (Vulkan 1.2) and dynamic
/// rendering (1.3). Format queries go to the driver live: they are cheap, and
/// there are hundreds of formats. Device selection, @ref Device::create and
/// @ref Device::adopt all read from one of these, so they agree on what a
/// device supports.
///
/// It also records the version usable on the device, which the spec bounds
/// by the instance as well as the device: a 1.3 device on a 1.2 instance may
/// use only 1.2. A newer version's features read as unsupported above that
/// version, so nothing checked against an info can enable them.
///
/// A default-constructed instance is empty (@ref handle is `VK_NULL_HANDLE`)
/// and answers no real query; it is a moved-from `Device`'s @ref Device::caps.
///
/// @warning The `VkPhysicalDevice` passed to @ref query must outlive every
///          format query, which re-enters the driver with the stored handle.
///
/// @code
/// const PhysicalDeviceInfo caps =
///     PhysicalDeviceInfo::query(physical, instance.api_version());
/// if (caps.api_version() >= VK_API_VERSION_1_3 &&
///     caps.supports_device_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
///   // ... a candidate for the renderer ...
/// }
/// @endcode
class VKC_VULKAN_API PhysicalDeviceInfo {
 public:
  /// @brief Capture the capabilities of @p physical.
  ///
  /// Queries only through what the usable version allows: below 1.1 (an
  /// embedder's 1.0 instance), the 1.0 feature query, and no newer feature.
  /// @param physical              The device to inspect.
  /// @param instance_api_version  The version the instance @p physical was
  ///                              enumerated from negotiated, e.g.
  ///                              @ref Instance::api_version.
  /// @return The captured capabilities.
  static PhysicalDeviceInfo query(VkPhysicalDevice physical,
                                  std::uint32_t instance_api_version);

  /// @brief Construct an empty info; @ref handle is `VK_NULL_HANDLE`.
  PhysicalDeviceInfo() = default;

  /// @return The inspected device (`VK_NULL_HANDLE` when empty).
  VkPhysicalDevice handle() const noexcept { return physical_; }

  /// @return The device's properties: `apiVersion`, `deviceType`,
  ///         `deviceName`, `limits` and the rest.
  const VkPhysicalDeviceProperties& properties() const noexcept {
    return properties_;
  }
  /// @return The device's limits (`properties().limits`).
  const VkPhysicalDeviceLimits& limits() const noexcept {
    return properties_.limits;
  }
  /// @return The Vulkan version usable on the device, without its patch: the
  ///         lower of what the device reports (`properties().apiVersion`)
  ///         and what its instance negotiated. Core functionality above it
  ///         must not be used.
  std::uint32_t api_version() const noexcept { return api_version_; }

  /// @return The memory heaps and types, which decide where an
  ///         @ref Allocator places each @ref MemoryUsage.
  const VkPhysicalDeviceMemoryProperties& memory_properties() const noexcept {
    return memory_properties_;
  }
  /// @brief Whether the GPU and the host share one memory.
  ///
  /// True when every heap is device-local; false for a discrete GPU with host
  /// memory beside its VRAM, and for an APU whose driver reports a VRAM
  /// carve-out beside host memory, whose device-local memory is the
  /// carve-out. On unified memory a `MemoryUsage::DeviceMapped` input costs a
  /// kernel no more than a device-only one, so a library may skip a staging
  /// copy there; otherwise kernel data belongs in `MemoryUsage::DeviceOnly`
  /// memory, reached from the host by staging.
  /// @return Whether memory is unified; when it is,
  ///         @ref device_mapped_memory is too.
  bool unified_memory() const noexcept { return unified_memory_; }
  /// @brief Whether the device has device-local memory the host maps
  ///        coherently, which `MemoryUsage::DeviceMapped` takes.
  ///
  /// True when a device-local memory type is host-visible and host-coherent,
  /// including a coherently mapped BAR window. Otherwise an allocator refuses
  /// `DeviceMapped` with
  /// `Unsupported`. Without it, data the host writes for shaders goes up by
  /// staging -- a `CommandBatch` upload into `DeviceOnly` memory.
  /// @return Whether `DeviceMapped` memory exists.
  bool device_mapped_memory() const noexcept { return device_mapped_memory_; }

  /// @return The queue families, indexed by family.
  const std::vector<VkQueueFamilyProperties>& queue_families() const noexcept {
    return queue_families_;
  }

  /// @param name  A device extension name.
  /// @return Whether the device supports @p name.
  bool supports_device_extension(const char* name) const;

  /// @return The supported core (1.0) features.
  const VkPhysicalDeviceFeatures& features() const noexcept {
    return features_;
  }
  /// @return Whether the device supports `timelineSemaphore` (Vulkan 1.2
  ///         core); `false` when @ref api_version is below 1.2.
  bool supports_timeline_semaphore() const noexcept {
    return timeline_semaphore_;
  }
  /// @return Whether the device supports `scalarBlockLayout` (Vulkan 1.2
  ///         core); `false` when @ref api_version is below 1.2.
  bool supports_scalar_block_layout() const noexcept {
    return scalar_block_layout_;
  }
  /// @return Whether the device supports `dynamicRendering` (Vulkan 1.3
  ///         core); `false` when @ref api_version is below 1.3.
  bool supports_dynamic_rendering() const noexcept {
    return dynamic_rendering_;
  }

  /// @brief The buffer, linear-tiling and optimal-tiling features of a format
  ///        (a live `vkGetPhysicalDeviceFormatProperties` query).
  /// @param format  The format to inspect.
  /// @return Its feature flags.
  VkFormatProperties format_properties(VkFormat format) const;

  /// @brief Whether @p format supports every bit of @p features for @p tiling.
  /// @param format    The format to test.
  /// @param tiling    `VK_IMAGE_TILING_OPTIMAL` tests the optimal-tiling
  ///                  features; anything else the linear-tiling ones.
  /// @param features  The feature bits required, all of them.
  /// @return `true` when every bit is supported.
  bool format_supports(VkFormat format, VkImageTiling tiling,
                       VkFormatFeatureFlags features) const;

 private:
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties_{};
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  bool unified_memory_ = false;
  bool device_mapped_memory_ = false;
  std::uint32_t api_version_ = 0;
  std::vector<VkQueueFamilyProperties> queue_families_;
  // Owned names, not VkExtensionProperties, so the info is self-contained and
  // holds no pointer into driver memory.
  std::vector<std::string> extension_names_;
  VkPhysicalDeviceFeatures features_{};
  bool timeline_semaphore_ = false;
  bool scalar_block_layout_ = false;
  bool dynamic_rendering_ = false;
};

}  // namespace volumetric_kit::core
