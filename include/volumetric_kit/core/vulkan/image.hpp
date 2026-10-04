// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file image.hpp
/// @brief A `VkImage`, its optional default view, and what backs them, owned
///        and freed together.

#include <cstdint>
#include <functional>
#include <optional>

#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief What an @ref Image is: everything recorded at creation that Vulkan
///        cannot be asked afterwards, and the layout its contents are in.
///
/// The defaults describe a single-sample, optimal-tiling 2D image, so an
/// adopter sets only what differs.
///
/// @code
/// ImageInfo info;  // a decoder's picture, imported by hand
/// info.image = imported;
/// info.format = VK_FORMAT_R8_UNORM;
/// info.extent = {1920, 1080, 1};
/// info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
/// info.layout = VK_IMAGE_LAYOUT_GENERAL;
/// Image picture(info, [=] { release(imported); });
/// @endcode
struct ImageInfo {
  /// The image.
  VkImage image = VK_NULL_HANDLE;
  /// A default view over every mip and layer, or `VK_NULL_HANDLE` for a
  /// viewless image (a transfer-only staging image, say).
  VkImageView view = VK_NULL_HANDLE;
  /// The create flags it was made with: `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`
  /// for a cubemap, which tells it from a six-layer array.
  VkImageCreateFlags flags = 0;
  /// 1D, 2D or 3D; a 3D image of depth 1 is not a 2D one.
  VkImageType type = VK_IMAGE_TYPE_2D;
  /// Its format.
  VkFormat format = VK_FORMAT_UNDEFINED;
  /// Its extent in texels; `depth` is 1 except for a 3D image.
  VkExtent3D extent{0, 0, 1};
  /// Its mip levels.
  std::uint32_t mip_levels = 1;
  /// Its array layers (6 for a cubemap).
  std::uint32_t array_layers = 1;
  /// Its sample count: above 1 for a multisampled image, which a copy cannot
  /// read and a resolve must.
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  /// Its tiling.
  VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
  /// The usage flags it was created with.
  VkImageUsageFlags usage = 0;
  /// The layout its contents are in: `VK_IMAGE_LAYOUT_UNDEFINED` for a fresh
  /// allocation, or the layout an adopter's maker left its contents in
  /// (`GENERAL` or `TRANSFER_SRC_OPTIMAL` for an image a copy reads). The
  /// owner records later transitions with @ref Image::set_layout.
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  /// The sharing mode it was created with.
  VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE;
  /// The memory type backing it, or empty when unknown (an adopted image
  /// whose maker did not say).
  std::optional<MemoryInfo> memory;
};

/// @brief Owns a `VkImage`, its optional default view, and what backs them,
///        freed together by a deleter.
///
/// The family's one image type -- recon's adopted decoder pictures and
/// gfx's textures, render targets and volumes alike. Made by
/// @ref Allocator::create_image, or adopted with the deleter its maker
/// supplies.
///
/// @warning The device the image was made on must outlive it. An image from an
///          @ref Allocator keeps the allocator's VMA state alive, as a
///          @ref Buffer does.
///
/// @code
/// ImageDesc desc;
/// desc.extent = {1280, 720};
/// desc.format = VK_FORMAT_R8G8B8A8_UNORM;
/// desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
/// VKC_ASSIGN(Image color, allocator.create_image(desc));
/// VkImageView view = color.view();
/// // ... submit an upload that leaves it SHADER_READ_ONLY_OPTIMAL ...
/// color.set_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
/// @endcode
class VKC_VULKAN_API Image {
 public:
  /// @brief Construct an empty image; @ref valid is false.
  Image() noexcept = default;

  /// @brief Adopt the image @p info describes, freed by @p deleter.
  /// @param info     The image, its view if any, and what it is.
  /// @param deleter  Frees the view, the image and what backs them, exactly
  ///                 once.
  Image(const ImageInfo& info, std::function<void()> deleter) noexcept;

  ~Image();
  Image(Image&& other) noexcept;
  Image& operator=(Image&& other) noexcept;
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  /// @return Everything recorded about the image (all defaults when empty).
  const ImageInfo& info() const noexcept { return info_; }
  /// @return The image (`VK_NULL_HANDLE` when empty).
  VkImage handle() const noexcept { return info_.image; }
  /// @return The default view, or `VK_NULL_HANDLE` for a viewless image.
  VkImageView view() const noexcept { return info_.view; }
  /// @return The create flags it was made with.
  VkImageCreateFlags create_flags() const noexcept { return info_.flags; }
  /// @return 1D, 2D or 3D.
  VkImageType type() const noexcept { return info_.type; }
  /// @return Its format.
  VkFormat format() const noexcept { return info_.format; }
  /// @return Its extent in texels.
  VkExtent3D extent() const noexcept { return info_.extent; }
  /// @return Its width in texels.
  std::uint32_t width() const noexcept { return info_.extent.width; }
  /// @return Its height in texels.
  std::uint32_t height() const noexcept { return info_.extent.height; }
  /// @return Its depth in texels (1 unless 3D).
  std::uint32_t depth() const noexcept { return info_.extent.depth; }
  /// @return Its mip levels.
  std::uint32_t mip_levels() const noexcept { return info_.mip_levels; }
  /// @return Its array layers.
  std::uint32_t array_layers() const noexcept { return info_.array_layers; }
  /// @return Its sample count.
  VkSampleCountFlagBits samples() const noexcept { return info_.samples; }
  /// @return Its tiling.
  VkImageTiling tiling() const noexcept { return info_.tiling; }
  /// @return The usage flags it was created with.
  VkImageUsageFlags usage() const noexcept { return info_.usage; }
  /// @return The layout its contents are in, as last recorded: at creation
  ///         or by @ref set_layout.
  VkImageLayout layout() const noexcept { return info_.layout; }
  /// @brief Record the layout the contents are now in, after the caller's
  ///        own transition.
  ///
  /// Only a record: it transitions nothing, and Vulkan cannot be asked for
  /// an image's layout. Code that records work on the image -- a copy, a
  /// descriptor write -- reads @ref layout, so update it once the
  /// transition's work is submitted, as it would then be stale otherwise.
  /// @param layout  The layout the contents are in.
  /// @pre @ref valid; an empty image records nothing.
  void set_layout(VkImageLayout layout) noexcept {
    if (valid()) info_.layout = layout;
  }
  /// @return The sharing mode it was created with.
  VkSharingMode sharing_mode() const noexcept { return info_.sharing; }
  /// @return The memory type backing it, when known.
  const std::optional<MemoryInfo>& memory_info() const noexcept {
    return info_.memory;
  }
  /// @return Whether the known backing memory is `DEVICE_LOCAL`.
  bool is_device_local() const noexcept {
    return info_.memory.has_value() &&
           (info_.memory->properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) !=
               0;
  }
  /// @return Whether this owns an image.
  bool valid() const noexcept { return info_.image != VK_NULL_HANDLE; }

 private:
  void destroy() noexcept;

  ImageInfo info_;
  std::function<void()> deleter_;
};

}  // namespace volumetric_kit::core
