// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file write_image.hpp
/// @brief Fill a test's image from host texels through a staging buffer, on
///        any device, with no test framework
///        (`volumetric_kit::core_test_policy`).
///
/// @ref volumetric_kit::core::CommandBatch reads images back but uploads only
/// buffers; a test that needs an image's contents -- a producer's picture, a
/// colour plane, a texture -- writes them with
/// @ref volumetric_kit::core::test::write_image.
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// ImageDesc desc;
/// desc.extent = {width, height};
/// desc.format = VK_FORMAT_R8G8B8A8_UNORM;
/// desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
///              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
/// VKC_ASSIGN(Image picture, allocator.create_image(desc));
/// VKC_TRY(test::write_image(device, allocator, picture, rgba.data(),
///                           rgba.size(), VK_IMAGE_LAYOUT_GENERAL));
/// // picture.layout() is GENERAL; a CommandBatch::copy reads it back.
/// @endcode

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

/// @brief Write tightly packed texels into the whole of mip 0, layer 0 of
///        @p dst on @p device's queue, leave every mip and layer in
///        @p layout, and wait for the fence: the writes are complete, and
///        available to a later submit on any queue.
///
/// The image's contents before are discarded, and the other mips and layers
/// hold undefined texels. On success the recorded layout
/// (@ref Image::layout) is @p layout, true of every subresource; on a
/// refusal, nothing is recorded or changed.
/// @param device     The queue it writes on, and its fence.
/// @param allocator  Makes the staging buffer, on @p device.
/// @param dst        A single-sample image of an uncompressed single-plane
///                   color format (@ref texel_bytes is not 0), with
///                   `TRANSFER_DST` usage, made on @p device, and usable from
///                   its queue family (`CONCURRENT` across families, or that
///                   family's alone). Whatever touched it before has
///                   finished.
/// @param texels     Width x height x depth texels, rows packed.
/// @param bytes      Their size: exactly the texels' count times
///                   @ref texel_bytes.
/// @param layout     The layout to leave it in: `GENERAL` or
///                   `TRANSFER_DST_OPTIMAL`; or, where @p dst's usage allows
///                   it, `TRANSFER_SRC_OPTIMAL` (`TRANSFER_SRC`),
///                   `SHADER_READ_ONLY_OPTIMAL` (`SAMPLED` or
///                   `INPUT_ATTACHMENT`) or `COLOR_ATTACHMENT_OPTIMAL`
///                   (`COLOR_ATTACHMENT`).
/// @return OK; @ref Status::Code::InvalidArgument for an empty image, one of
///         another format, sample count or usage, a byte count that is not
///         the image's, null texels, or another layout; or a staging
///         allocation's or a submit's failure.
Status write_image(const Device& device, Allocator& allocator, Image& dst,
                   const void* texels, VkDeviceSize bytes,
                   VkImageLayout layout);

}  // namespace volumetric_kit::core::test
