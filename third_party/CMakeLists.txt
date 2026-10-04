# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies. The base tier has none. The vulkan tier takes Vulkan
# from the system (src/volumetric_kit/core/vulkan/) and vendors VMA here.
# googletest is vendored too, and only when the test suite is built.
#
# TODO: as tiers land, add their dependencies here, mirroring recon's hybrid
# system/vendored strategy: Eigen for the camera tier, and the Orbbec SDK
# (optional) for the sensor tier.

include(FetchContent)

# --- VMA: Vulkan Memory Allocator (header-only; one core_vulkan TU) ----------
# The same pin and FetchContent name as recon and gfx, so a build fetching
# several of them resolves one copy -- and the libraries sharing a VkDevice
# allocate through one VMA version. It defines GPUOpen::VulkanMemoryAllocator,
# whose include directory core_vulkan reads as a PRIVATE SYSTEM include, never
# part of its installed interface: VMA stays behind the allocator's pimpl.
if(VKC_WITH_VULKAN)
  FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY
      https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(VulkanMemoryAllocator)
endif()

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
