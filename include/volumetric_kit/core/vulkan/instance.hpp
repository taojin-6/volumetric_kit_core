// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file instance.hpp
/// @brief The Vulkan instance, optional validation, and physical-device
///        selection against a library's requirements.

#include <cstdint>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Parameters for @ref Instance::create.
///
/// @code
/// InstanceConfig config;
/// config.app_name = "scanner";
/// config.enable_validation = true;  // a debug build
/// config.extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
/// VKC_ASSIGN(Instance instance, Instance::create(config));
/// @endcode
struct InstanceConfig {
  /// The application name reported to the driver in `VkApplicationInfo`.
  std::string app_name = "volumetric_kit";
  /// Enable the Khronos validation layer when it is installed and loads (a
  /// logged warning, not a failure, when it does not; ask
  /// @ref Instance::validation_enabled). Its messages reach the log sink
  /// with source `"vulkan"`.
  bool enable_validation = false;
  /// Enable `VK_EXT_debug_utils` whenever the loader offers it, independently
  /// of @ref enable_validation.
  ///
  /// On by default because the extension is what lets a GPU profiler name
  /// what it shows: a @ref Device resolves its object-name and label entry
  /// points, so dispatches and buffers carry their names in Nsight, and in an
  /// Xcode capture through MoltenVK's Metal debug groups. Without a profiler
  /// attached, the entry points are driver stubs and cost a branch. Clear it
  /// to hold a shipping instance to the extensions it strictly needs -- unless
  /// validation is on and the layer present, since its messenger needs the
  /// same extension, which then stays enabled (a line is logged saying so).
  bool request_debug_utils = true;
  /// Further instance extensions, e.g. a platform's surface extensions.
  std::vector<const char*> extensions;
};

/// @brief Owns a `VkInstance`, and its debug messenger under validation.
///
/// Split from @ref Device so a headless compute device, several devices, or a
/// device shared between libraries all compose on one instance. Portability
/// enumeration is enabled where the loader offers it, so MoltenVK devices are
/// visible on Apple.
///
/// The instance asks for Vulkan 1.3, or the loader's version when that is
/// lower, and never less than 1.1 (`vkGetPhysicalDeviceFeatures2`). It asks
/// for the highest version on purpose: MoltenVK caps every device's reported
/// version at whatever the instance requested, so an instance asking for 1.2
/// would hide a 1.3 device from a library that needs 1.3.
///
/// @code
/// VKC_ASSIGN(Instance instance, Instance::create({}));
/// DeviceRequirements reqs;
/// reqs.scalar_block_layout = true;
/// VKC_ASSIGN(PhysicalDeviceInfo gpu, instance.select_physical_device(reqs));
/// log_message(LogLevel::Info, "app", gpu.properties().deviceName);
/// @endcode
class VKC_VULKAN_API Instance {
 public:
  /// @brief Create the instance.
  /// @param config  App name, validation, debug utils and extra extensions.
  /// @return The instance; @ref Status::Code::Unsupported for a loader below
  ///         Vulkan 1.1; or the failed `vkCreateInstance`'s `VkResult` as a
  ///         backend @ref Status.
  static Result<Instance> create(const InstanceConfig& config);

  ~Instance();
  Instance(Instance&& other) noexcept;
  Instance& operator=(Instance&& other) noexcept;
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  /// @return The owned `VkInstance` (`VK_NULL_HANDLE` when moved-from).
  VkInstance handle() const noexcept { return instance_; }
  /// @return The Vulkan version the instance negotiated, which bounds every
  ///         device made on it.
  std::uint32_t api_version() const noexcept { return api_version_; }
  /// @return Whether the Khronos validation layer is enabled.
  bool validation_enabled() const noexcept { return validation_enabled_; }
  /// @return Whether the validation layer's messages reach the log sink: it
  ///         is enabled, and its messenger was created. An enabled layer
  ///         without one prints to its own output, which no handler sees.
  bool validation_logged() const noexcept {
    return messenger_ != VK_NULL_HANDLE;
  }
  /// @return Whether `VK_EXT_debug_utils` is enabled, which a @ref Device
  ///         needs to resolve its label entry points.
  bool debug_utils_enabled() const noexcept { return debug_utils_enabled_; }

  /// @brief Pick the best physical device that meets @p reqs: a discrete GPU
  ///        over an integrated one, over a virtual one, over a CPU (lavapipe).
  /// @param reqs     The requirements, checked with @ref check_device_support
  ///                 against each device's capabilities on this instance.
  /// @param surface  The surface to present to; required when
  ///                 @ref DeviceRequirements::needs_present.
  /// @return The device's capabilities as the check saw them, which
  ///         @ref Device::create takes, so it need not query them again;
  ///         @ref Status::Code::Unsupported, naming why each device was
  ///         refused, when none qualifies; or
  ///         @ref Status::Code::InvalidArgument for a missing @p surface.
  Result<PhysicalDeviceInfo> select_physical_device(
      const DeviceRequirements& reqs = {},
      VkSurfaceKHR surface = VK_NULL_HANDLE) const;

 private:
  Instance() = default;
  void destroy() noexcept;

  VkInstance instance_ = VK_NULL_HANDLE;
  // The validation messenger, when validation is on and
  // VK_EXT_debug_utils offered; destroyed before the instance.
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  std::uint32_t api_version_ = 0;
  bool validation_enabled_ = false;
  bool debug_utils_enabled_ = false;
};

}  // namespace volumetric_kit::core
