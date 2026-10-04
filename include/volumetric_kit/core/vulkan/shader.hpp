// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file shader.hpp
/// @brief A `VkShaderModule` built from SPIR-V.

#include <cstddef>
#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Owns a `VkShaderModule` built from SPIR-V words.
///
/// Takes SPIR-V as 32-bit words and a byte length; it reads no files. Callers
/// embed the compiled `.spv` at build time or read it themselves. No
/// reflection: descriptor layouts and push-constant ranges are declared where
/// a pipeline is made, so the core needs no SPIR-V parser. gfx, which builds
/// its graphics pipelines from reflection, wraps this module with its own.
///
/// @warning The device passed to @ref create must outlive the module.
///
/// @code
/// VKC_ASSIGN(ShaderModule shader,
///            ShaderModule::create(device.handle(), kIntegrateSpv,
///                                 sizeof(kIntegrateSpv)));
/// @endcode
class VKC_VULKAN_API ShaderModule {
 public:
  /// @brief Create a module from SPIR-V.
  /// @param device      The device.
  /// @param code        The SPIR-V words.
  /// @param size_bytes  Their length in bytes: non-zero, a multiple of 4.
  /// @return The module; @ref Status::Code::InvalidArgument for a null
  ///         @p device or @p code, or a bad size; or a backend @ref Status.
  static Result<ShaderModule> create(VkDevice device, const std::uint32_t* code,
                                     std::size_t size_bytes);

  /// @brief Construct an empty module; @ref valid is false.
  ShaderModule() noexcept = default;
  ShaderModule(ShaderModule&&) noexcept = default;
  ShaderModule& operator=(ShaderModule&&) noexcept = default;
  ShaderModule(const ShaderModule&) = delete;
  ShaderModule& operator=(const ShaderModule&) = delete;
  ~ShaderModule() = default;

  /// @return The module (`VK_NULL_HANDLE` when empty).
  VkShaderModule handle() const noexcept { return module_.get(); }
  /// @return Whether this owns a module.
  bool valid() const noexcept { return module_.valid(); }

 private:
  UniqueHandle<VkShaderModule, vkDestroyShaderModule> module_;
};

}  // namespace volumetric_kit::core
