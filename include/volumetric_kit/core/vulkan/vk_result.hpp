// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file vk_result.hpp
/// @brief A failed `VkResult` as a backend-neutral @ref
/// volumetric_kit::core::Status,
///        and back.
///
/// The base tier's `Status` carries a backend's code as a plain `int64_t`
/// (domain `Code::Backend`), so it includes no GPU API. This header is the
/// one place the vulkan tier turns a `VkResult` into that `Status` -- with
/// @ref vk_error and @ref VKC_VK_TRY -- and reads it back with @ref vk_result
/// and @ref to_string(VkResult).
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
/// @return A non-OK `Status`, domain `Code::Backend`, whose @ref Status::detail
///         is @p result.
inline Status vk_error(VkResult result, std::string_view what) {
  return Status::backend_error(static_cast<std::int64_t>(result),
                               std::string(what));
}

/// @brief The `VkResult` a backend @ref Status carries.
/// @param status  Any status.
/// @return Its @ref Status::detail as a `VkResult` when its domain is
///         `Code::Backend` and the detail fits in 32 bits; empty otherwise,
///         success included.
///
/// The domain does not say *which* backend failed: a CUDA failure is a backend
/// status too, whose `cudaError_t` detail this reads as an unrelated
/// `VkResult` (`cudaErrorMemoryAllocation`, 2, as `VK_TIMEOUT`). Ask it only of
/// a status from a Vulkan call.
inline std::optional<VkResult> vk_result(const Status& status) noexcept {
  // TODO: return empty for a CUDA status once Status records which backend
  // failed.
  const std::int64_t detail = status.detail();
  // A detail wider than VkResult's 32 bits is no VkResult, and converting it
  // to the enum would be undefined.
  if (status.domain() != Status::Code::Backend ||
      detail < std::numeric_limits<std::int32_t>::min() ||
      detail > std::numeric_limits<std::int32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<VkResult>(detail);
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
