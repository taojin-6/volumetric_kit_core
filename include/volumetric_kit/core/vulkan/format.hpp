// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file format.hpp
/// @brief What a `VkFormat` implies for a view and a copy: its depth and
///        stencil aspects, the aspect a default view covers, whether a view
///        needs a sampler Y'CbCr conversion, and the bytes of one texel.
///
/// The tier's own image code and its consumers' share these, so the core
/// needs no Vulkan-Utility-Libraries, whose format helpers require headers
/// newer than some systems ship (DECISIONS.md, "Vulkan headers come from the
/// system"). They cover the formats of core Vulkan and its KHR extensions,
/// whatever headers the core was built with; any other extension's format --
/// a vendor's, or an EXT's -- reads as no depth, no stencil, no conversion and
/// 0 bytes, so a caller refuses it rather than mis-sizes it.
///
/// @code
/// const std::uint32_t texel = texel_bytes(image.format());
/// if (texel == 0) return Status::unsupported("no flat copy of this format");
/// const VkDeviceSize bytes = VkDeviceSize{width} * height * texel;
/// @endcode

#include <cstdint>

#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Whether @p format has a depth aspect.
/// @param format  Any format.
/// @return `true` for the depth-only and combined depth/stencil formats.
VKC_VULKAN_API bool format_has_depth(VkFormat format) noexcept;

/// @brief Whether @p format has a stencil aspect.
/// @param format  Any format.
/// @return `true` for `VK_FORMAT_S8_UINT` and the combined depth/stencil
///         formats.
VKC_VULKAN_API bool format_has_stencil(VkFormat format) noexcept;

/// @brief The aspect a default view of an image of @p format covers.
///
/// A combined depth/stencil format gives depth: one view cannot sample both,
/// and depth is the aspect sampled and attached. A stencil view is made
/// explicitly.
/// @param format  Any format.
/// @return `VK_IMAGE_ASPECT_DEPTH_BIT` for the depth-only and combined
///         formats, `VK_IMAGE_ASPECT_STENCIL_BIT` for `VK_FORMAT_S8_UINT`, and
///         `VK_IMAGE_ASPECT_COLOR_BIT` otherwise.
VKC_VULKAN_API VkImageAspectFlags view_aspect(VkFormat format) noexcept;

/// @brief Whether a COLOR view of an image of @p format must carry a sampler
///        Y'CbCr conversion (VUID-VkImageViewCreateInfo-format-06415).
///
/// These are the formats the registry (vk.xml) gives a chroma attribute:
/// multi-planar, 4:2:2, and the RGBA 4PACK16 ones -- such as a video
/// decoder's NV12 picture. The one- and two-component R10X6 / R12X4 formats
/// need none.
/// @param format  Any format.
/// @return `true` for a format whose view needs the conversion.
VKC_VULKAN_API bool format_needs_ycbcr_conversion(VkFormat format) noexcept;

/// @brief The bytes of one texel of an uncompressed, single-plane color
///        format: the stride of a tightly packed image-to-buffer copy.
/// @param format  Any format.
/// @return The texel's size, or 0 for a format a flat per-texel copy cannot
///         size -- undefined, depth/stencil, compressed, multi-planar or 4:2:2
///         -- or one of an extension other than KHR.
VKC_VULKAN_API std::uint32_t texel_bytes(VkFormat format) noexcept;

}  // namespace volumetric_kit::core
