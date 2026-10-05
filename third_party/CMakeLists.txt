# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies. The base tier has none. The vulkan tier takes Vulkan
# from the system (src/volumetric_kit/core/vulkan/) and vendors VMA here; the
# camera tier vendors nlohmann/json. googletest is vendored too, and only when
# the test suite is built.
#
# TODO: add the sensor tier's dependency as it lands, mirroring recon's hybrid
# system/vendored strategy: the Orbbec SDK (optional, installed).

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

# --- nlohmann/json (header-only; parsed in one core_camera TU) ---------------
# The rig calibration file is JSON. The release's source archive, hash-pinned,
# under the FetchContent name and pin recon uses, so a build fetching both
# resolves one copy. Only its include/ tree is read, so its CMake is not run
# (SOURCE_SUBDIR names a directory the archive does not have), and core_camera
# reads it as a PRIVATE SYSTEM include, never part of its installed interface.
if(VKC_WITH_CAMERA)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW) # extracted files get the extraction time
  endif()
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH
      SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SOURCE_SUBDIR do-not-add-json-cmake)
  FetchContent_MakeAvailable(nlohmann_json)
  set(VKC_JSON_INCLUDE_DIR
      "${nlohmann_json_SOURCE_DIR}/include"
      PARENT_SCOPE)
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
