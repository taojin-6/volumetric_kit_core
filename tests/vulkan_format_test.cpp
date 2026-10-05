// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// What a VkFormat implies for a view and a copy. Needs no device.

#include "volumetric_kit/core/vulkan/format.hpp"

#include <gtest/gtest.h>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

TEST(Format, DepthAndStencilAspects) {
  for (const VkFormat depth :
       {VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32,
        VK_FORMAT_D32_SFLOAT}) {
    EXPECT_TRUE(format_has_depth(depth)) << depth;
    EXPECT_FALSE(format_has_stencil(depth)) << depth;
  }
  for (const VkFormat both :
       {VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    EXPECT_TRUE(format_has_depth(both)) << both;
    EXPECT_TRUE(format_has_stencil(both)) << both;
  }
  EXPECT_FALSE(format_has_depth(VK_FORMAT_S8_UINT));
  EXPECT_TRUE(format_has_stencil(VK_FORMAT_S8_UINT));
  EXPECT_FALSE(format_has_depth(VK_FORMAT_R32_SFLOAT));
  EXPECT_FALSE(format_has_stencil(VK_FORMAT_R8_UINT));
}

// A combined format's default view is its depth: one view cannot sample both.
TEST(Format, ADefaultViewOfACombinedFormatIsItsDepth) {
  EXPECT_EQ(view_aspect(VK_FORMAT_D32_SFLOAT), VK_IMAGE_ASPECT_DEPTH_BIT);
  EXPECT_EQ(view_aspect(VK_FORMAT_D24_UNORM_S8_UINT),
            VK_IMAGE_ASPECT_DEPTH_BIT);
  EXPECT_EQ(view_aspect(VK_FORMAT_S8_UINT), VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_EQ(view_aspect(VK_FORMAT_R8G8B8A8_SRGB), VK_IMAGE_ASPECT_COLOR_BIT);
  // A multi-planar format is color too; its planes are views of their own.
  EXPECT_EQ(view_aspect(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM),
            VK_IMAGE_ASPECT_COLOR_BIT);
}

TEST(Format, TexelBytesOfUncompressedColorFormats) {
  EXPECT_EQ(texel_bytes(VK_FORMAT_R8_UNORM), 1U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R4G4_UNORM_PACK8), 1U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R16_SFLOAT), 2U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_A4B4G4R4_UNORM_PACK16), 2U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R10X6_UNORM_PACK16), 2U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R8G8B8_SRGB), 3U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_B8G8R8_SRGB), 3U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_A8B8G8R8_SRGB_PACK32), 4U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_A2B10G10R10_UNORM_PACK32), 4U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32), 4U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R16G16B16_SFLOAT), 6U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R16G16B16A16_SFLOAT), 8U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R32G32B32_SFLOAT), 12U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R32G32B32A32_SFLOAT), 16U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R64G64B64_SFLOAT), 24U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_R64G64B64A64_SFLOAT), 32U);
}

// A format a flat per-texel copy cannot size reads 0, so a caller refuses it
// rather than under-allocates.
TEST(Format, TexelBytesIsZeroWhereAFlatCopyCannotSize) {
  EXPECT_EQ(texel_bytes(VK_FORMAT_UNDEFINED), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_D32_SFLOAT), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_D24_UNORM_S8_UINT), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_S8_UINT), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_BC1_RGB_UNORM_BLOCK), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_ASTC_4x4_SRGB_BLOCK), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM), 0U);
  EXPECT_EQ(texel_bytes(VK_FORMAT_G8B8G8R8_422_UNORM), 0U);  // 2x1 blocks
}

}  // namespace
}  // namespace volumetric_kit::core
