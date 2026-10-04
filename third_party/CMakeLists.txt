# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies. The base tier has none; googletest is vendored via
# pinned FetchContent, and only when the test suite is built.
#
# TODO: as tiers land, add their dependencies here, mirroring recon's hybrid
# system/vendored strategy: Vulkan-Headers + VMA for the vulkan tier, Eigen for
# the camera tier, and the Orbbec SDK (optional) for the sensor tier.

include(FetchContent)

if(VKC_BUILD_TESTS)
  # The tests use gtest alone: no gmock to build, nothing to install. Plain
  # variables reach googletest's option()s (CMP0077 is NEW there) without
  # forcing a cache entry that would override a parent project's choice.
  set(BUILD_GMOCK OFF)
  set(INSTALL_GTEST OFF)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()
