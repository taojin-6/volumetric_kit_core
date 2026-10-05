// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// format.hpp against Khronos' Vulkan-Utility-Libraries, for every format the
// headers name, where its headers are installed beside them (the Vulkan SDK;
// Homebrew's vulkan-utility-libraries). Needs no device. Its own file: the
// one place outside vulkan.hpp that includes Vulkan's headers by name
// (.pre-commit-config.yaml, vulkan-include-umbrella).

#include <gtest/gtest.h>

#include "volumetric_kit/core/vulkan/format.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

#if __has_include(<vulkan/utility/vk_format_utils.h>) && \
    __has_include(<vulkan/vk_enum_string_helper.h>)
#define VKC_TEST_HAS_VULKAN_UTILITY 1
#include <vulkan/utility/vk_format_utils.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#endif

namespace volumetric_kit::core {
namespace {

#ifdef VKC_TEST_HAS_VULKAN_UTILITY

// Every value the headers name: the core formats, then each extension's block
// of values from 1000000000 + (extension number - 1) * 1000.
std::vector<VkFormat> named_formats() {
  std::vector<VkFormat> formats;
  auto add = [&](std::int32_t value) {
    const auto format = static_cast<VkFormat>(value);
    if (std::strcmp(string_VkFormat(format), "Unhandled VkFormat") != 0) {
      formats.push_back(format);
    }
  };
  for (std::int32_t value = 0; value < 1000; ++value) add(value);
  for (std::int32_t value = 1000000000; value < 1001000000; ++value) {
    add(value);
  }
  return formats;
}

// Whether format.hpp covers @p name: a core format's name ends in its numeric
// format or packing, a KHR one's in KHR. Any other extension's -- an EXT's or
// a vendor's -- ends in its author's tag.
bool covered(const std::string& name) {
  const std::string tail = name.substr(name.rfind('_') + 1);
  if (tail == "KHR" || tail.find("PACK") != std::string::npos) return true;
  constexpr std::array<std::string_view, 11> kNumeric = {
      "UNDEFINED", "UNORM", "SNORM",  "USCALED", "SSCALED", "UINT",
      "SINT",      "SRGB",  "SFLOAT", "UFLOAT",  "BLOCK"};
  return std::find(kNumeric.begin(), kNumeric.end(), tail) != kNumeric.end();
}

TEST(FormatReference, AgreesWithVulkanUtilityLibraries) {
  const std::vector<VkFormat> formats = named_formats();
  ASSERT_GT(formats.size(), 200U) << "the enumeration found too few formats";
  for (const VkFormat format : formats) {
    const std::string name = string_VkFormat(format);
    if (!covered(name)) {
      // Reads as nothing, so a caller refuses it (format.hpp).
      EXPECT_FALSE(format_has_depth(format)) << name;
      EXPECT_FALSE(format_has_stencil(format)) << name;
      EXPECT_FALSE(format_needs_ycbcr_conversion(format)) << name;
      EXPECT_EQ(texel_bytes(format), 0U) << name;
      continue;
    }
    // A new core or KHR format in newer headers fails here until format.cpp
    // learns it.
    const bool depth = vkuFormatHasDepth(format);
    const bool stencil = vkuFormatHasStencil(format);
    EXPECT_EQ(format_has_depth(format), depth) << name;
    EXPECT_EQ(format_has_stencil(format), stencil) << name;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth) {
      aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (stencil) {
      aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    EXPECT_EQ(view_aspect(format), aspect) << name;
    EXPECT_EQ(format_needs_ycbcr_conversion(format),
              vkuFormatRequiresYcbcrConversion(format))
        << name;
    // A texel of a flat copy is a block of one texel: color, single-plane,
    // neither compressed nor 4:2:2 (whose blocks span texels).
    const std::uint32_t texel =
        vkuFormatIsColor(format) && !vkuFormatIsBlockedImage(format)
            ? vkuFormatTexelBlockSize(format)
            : 0U;
    EXPECT_EQ(texel_bytes(format), texel) << name;
  }
}

#else

TEST(FormatReference, AgreesWithVulkanUtilityLibraries) {
  GTEST_SKIP() << "Vulkan-Utility-Libraries' headers are not installed beside "
                  "the Vulkan headers";
}

#endif

}  // namespace
}  // namespace volumetric_kit::core
