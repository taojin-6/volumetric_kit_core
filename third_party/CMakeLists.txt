# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies. The base tier has none. The vulkan tier takes Vulkan
# from the system (src/volumetric_kit/core/vulkan/) and vendors VMA here.
# googletest is vendored too, and only when the test suite is built.

include(FetchContent)

# --- VMA: Vulkan Memory Allocator (header-only; one core_vulkan TU) ----------
# The same pin and FetchContent name as recon and gfx, so a build fetching
# several of them resolves one copy -- and the libraries sharing a VkDevice
# allocate through one VMA version. It defines GPUOpen::VulkanMemoryAllocator,
# whose include directory core_vulkan reads as a PRIVATE SYSTEM include, never
# part of its public interface: VMA stays behind the allocator's pimpl.
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

# Config is optional; base-only consumers fetch no JSON dependency. The name and
# hash match recon so a family build resolves one copy.
if(VKC_WITH_CONFIG)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
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
