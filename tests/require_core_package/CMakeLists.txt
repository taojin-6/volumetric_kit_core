# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a project that finds a sibling's installed package. The
# sibling's package config (vkc_siblingConfig.cmake) re-finds the core on
# CMAKE_PREFIX_PATH and requires it, as gfx's and recon's do. Configured by
# tests/CMakeLists.txt:
#
# -DCMAKE_PREFIX_PATH=<core prefix> -DVKC_SIBLING_REQUIRE=<version>
# [-DVKC_SIBLING_REQUIRE_VULKAN=ON] [-DVKC_SIBLING_REQUIRED=ON]
# [-DVKC_SIBLING_CORE_FIRST=find_package|subdirectory, the second with
# -DVKC_SOURCE_DIR=<checkout> and the core's options]
#
# It says whether it found the sibling, and if not, why. With
# VKC_SIBLING_CORE_FIRST it adds the core before the sibling -- found twice,
# with different arguments, or from source -- and requires it after.
cmake_minimum_required(VERSION 3.21)
project(vkc_sibling_consumer LANGUAGES CXX)

set(VKC_SIBLING_REQUIRE
    "0.1.0"
    CACHE STRING "The oldest volumetric_kit_core the sibling builds with")
option(VKC_SIBLING_REQUIRE_VULKAN "The sibling requires the core's vulkan tier"
       OFF)
option(VKC_SIBLING_REQUIRED "Find the sibling REQUIRED, not QUIET" OFF)
set(VKC_SIBLING_CORE_FIRST
    ""
    CACHE STRING
          "Add the core before the sibling: find_package, subdirectory or empty"
)

if(VKC_SIBLING_CORE_FIRST STREQUAL "find_package")
  find_package(volumetric_kit_core CONFIG REQUIRED)
  find_package(volumetric_kit_core CONFIG)
elseif(VKC_SIBLING_CORE_FIRST STREQUAL "subdirectory")
  # As FetchContent adds it, without redirecting find_package: the sibling's
  # config then reads the installed core's package.
  add_subdirectory("${VKC_SOURCE_DIR}" volumetric_kit_core)
elseif(VKC_SIBLING_CORE_FIRST)
  message(FATAL_ERROR "VKC_SIBLING_CORE_FIRST must be find_package, "
                      "subdirectory or empty")
endif()

set(_vkc_mode QUIET)
if(VKC_SIBLING_REQUIRED)
  set(_vkc_mode REQUIRED)
endif()
find_package(vkc_sibling CONFIG ${_vkc_mode} PATHS
             "${CMAKE_CURRENT_SOURCE_DIR}" NO_DEFAULT_PATH)
if(vkc_sibling_FOUND)
  message(STATUS "vkc_sibling found")
else()
  message(STATUS "vkc_sibling not found: ${vkc_sibling_NOT_FOUND_MESSAGE}")
endif()

if(VKC_SIBLING_CORE_FIRST)
  vkc_require_core(${VKC_SIBLING_REQUIRE})
  message(STATUS "vkc_sibling_consumer required the core itself")
endif()
