// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file vk_result.hpp
/// @brief A failed `VkResult` as a backend-neutral @ref
/// volumetric_kit::core::Status,
///        and back.
///
/// The base tier's `Status` carries a backend's code as a plain `int64_t`
/// (domain `Code::Backend`), tagged with the backend, so it includes no GPU
/// API. This header is the one place the vulkan tier turns a `VkResult` into
/// that `Status` -- with @ref vk_error and @ref VKC_VK_TRY -- and reads it back
/// with @ref vk_result and @ref to_string(VkResult).
///
/// @code
/// Status init(VkDevice device, VkFence* fence) {
///   VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
///   VKC_VK_TRY(vkCreateFence(device, &info, nullptr, fence));
///   return {};
/// }
/// @endcode

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Wrap a failed `VkResult` as a backend @ref Status.
/// @param result  What the failed Vulkan call returned.
/// @param what    Context for the message, e.g. the failing call.
/// @pre @p result is not `VK_SUCCESS`: a success code is no failure, and
///      @ref Status::backend_error aborts on one.
/// @return A non-OK `Status`, domain `Code::Backend`, whose
///         @ref Status::backend is `Vulkan` and whose @ref Status::detail is
///         @p result.
inline Status vk_error(VkResult result, std::string_view what) {
  return Status::backend_error(Status::Backend::Vulkan,
                               static_cast<std::int64_t>(result),
                               std::string(what));
}

/// @brief The `VkResult` a Vulkan @ref Status carries.
/// @param status  Any status.
/// @return Its @ref Status::detail as a `VkResult` when its backend is
///         `Vulkan` and the detail is in `int32_t`'s range, which is
///         `VkResult`'s; empty otherwise -- for success, another domain, and
///         another backend's failure, whose code (`CUDA_ERROR_OUT_OF_MEMORY`,
///         2) would read as an unrelated `VkResult` (`VK_TIMEOUT`). A 32-bit
///         code stored without sign extension (from a `uint32_t`) is outside
///         that range: store a `VkResult` with @ref vk_error.
inline std::optional<VkResult> vk_result(const Status& status) noexcept {
  if (status.backend() != Status::Backend::Vulkan) return std::nullopt;
  // Converting a code outside VkResult's range, int32_t's, to the enum would
  // be undefined.
  const std::int64_t code = status.detail();
  if (code < std::numeric_limits<std::int32_t>::min() ||
      code > std::numeric_limits<std::int32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<VkResult>(code);
}

/// @brief The name of a `VkResult`, e.g. `"VK_ERROR_DEVICE_LOST"`.
/// @param result  Any `VkResult`.
/// @return A static `string_view`; `"VK_RESULT_UNKNOWN"` for a code the table
///         does not name.
VKC_VULKAN_API std::string_view to_string(VkResult result) noexcept;

}  // namespace volumetric_kit::core

/// @brief Evaluate a `VkResult` expression and early-return a backend
///        `Status` unless it is `VK_SUCCESS`.
/// @param expr  An expression yielding a `VkResult`; its text becomes the
///              failure's message, so the failing call names itself.
///
/// Usable only inside a function returning `Status` or `Result<T>`.
/// @warning Only for calls whose sole success code is `VK_SUCCESS`: it treats
///          `VK_SUBOPTIMAL_KHR`, `VK_INCOMPLETE`, `VK_NOT_READY` and
///          `VK_TIMEOUT` as failures. Check a call with several success codes
///          by hand.
#define VKC_VK_TRY(expr)                                       \
  do {                                                         \
    const VkResult _vkc_vk = (expr);                           \
    if (_vkc_vk != VK_SUCCESS) {                               \
      return ::volumetric_kit::core::vk_error(_vkc_vk, #expr); \
    }                                                          \
  } while (0)
