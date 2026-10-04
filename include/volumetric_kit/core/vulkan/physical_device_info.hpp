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
/// @ref query captures the properties and limits, the supported device
/// extensions, the queue families, the core (1.0) features, and the three
/// newer features the family's libraries require -- timeline semaphores and
/// scalar block layout (Vulkan 1.2) and dynamic rendering (1.3). Format
/// queries go to the driver live: they are cheap, and there are hundreds of
/// formats. Device selection, @ref Device::create and @ref Device::adopt all
/// read from one of these, so they agree on what a device supports.
///
/// A default-constructed instance is empty (@ref handle is `VK_NULL_HANDLE`)
/// and answers no real query; it is a moved-from `Device`'s @ref Device::caps.
///
/// @warning The `VkPhysicalDevice` passed to @ref query must outlive every
///          format query, which re-enters the driver with the stored handle.
///
/// @code
/// const PhysicalDeviceInfo caps = PhysicalDeviceInfo::query(physical);
/// if (caps.api_version() >= VK_API_VERSION_1_3 &&
///     caps.supports_device_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
///   // ... a candidate for the renderer ...
/// }
/// @endcode
class VKC_VULKAN_API PhysicalDeviceInfo {
 public:
  /// @brief Capture the capabilities of @p physical.
  /// @param physical  The device to inspect, enumerated from an instance that
  ///                  negotiated Vulkan 1.1 or later
  ///                  (`vkGetPhysicalDeviceFeatures2` is 1.1 core).
  /// @return The captured capabilities.
  static PhysicalDeviceInfo query(VkPhysicalDevice physical);

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
  /// @return The Vulkan version the device reports (`properties().apiVersion`).
  std::uint32_t api_version() const noexcept { return properties_.apiVersion; }

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
  ///         core); `false` below 1.2.
  bool supports_timeline_semaphore() const noexcept {
    return timeline_semaphore_;
  }
  /// @return Whether the device supports `scalarBlockLayout` (Vulkan 1.2
  ///         core); `false` below 1.2.
  bool supports_scalar_block_layout() const noexcept {
    return scalar_block_layout_;
  }
  /// @return Whether the device supports `dynamicRendering` (Vulkan 1.3
  ///         core); `false` below 1.3.
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
