// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/version.hpp"

#include <string>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

TEST(Version, LinkedLibraryMatchesTheHeaders) {
  EXPECT_EQ(version_major(), VKC_VERSION_MAJOR);
  EXPECT_EQ(version_minor(), VKC_VERSION_MINOR);
  EXPECT_EQ(version_patch(), VKC_VERSION_PATCH);
  EXPECT_EQ(std::string(version_string()), std::string(VKC_VERSION_STRING));
}

TEST(Version, StringIsMajorMinorPatch) {
  const std::string want = std::to_string(VKC_VERSION_MAJOR) + "." +
                           std::to_string(VKC_VERSION_MINOR) + "." +
                           std::to_string(VKC_VERSION_PATCH);
  EXPECT_EQ(std::string(version_string()), want);
}

}  // namespace
}  // namespace volumetric_kit::core
