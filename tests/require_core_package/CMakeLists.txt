# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a project that finds a sibling's installed package. The
# sibling's package config (vkc_siblingConfig.cmake) re-finds the core on
# CMAKE_PREFIX_PATH and requires it, as gfx's and recon's do. Configured by
# tests/CMakeLists.txt:
#
# -DCMAKE_PREFIX_PATH=<core prefix> -DVKC_SIBLING_REQUIRE=<version>
# [-DVKC_SIBLING_REQUIRE_VULKAN=ON] [-DVKC_SIBLING_REQUIRED=ON]
#
# It says whether it found the sibling, and if not, why.
cmake_minimum_required(VERSION 3.21)
project(vkc_sibling_consumer LANGUAGES CXX)

set(VKC_SIBLING_REQUIRE
    "0.1.0"
    CACHE STRING "The oldest volumetric_kit_core the sibling builds with")
option(VKC_SIBLING_REQUIRE_VULKAN "The sibling requires the core's vulkan tier"
       OFF)
option(VKC_SIBLING_REQUIRED "Find the sibling REQUIRED, not QUIET" OFF)

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
