# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in sibling at 0.3.0: its umbrella, volumetric_kit::sibling, carries
# its version as a sibling's does (unless VKC_SIBLING_UNVERSIONED), and it has
# one more target, volumetric_kit::sibling_part.
cmake_minimum_required(VERSION 3.21)
project(
  volumetric_kit_sibling
  VERSION 0.3.0
  LANGUAGES NONE)

add_library(volumetric_kit_sibling INTERFACE)
add_library(volumetric_kit::sibling ALIAS volumetric_kit_sibling)
if(NOT VKC_SIBLING_UNVERSIONED)
  set_target_properties(volumetric_kit_sibling PROPERTIES VKC_PACKAGE_VERSION
                                                          "${PROJECT_VERSION}")
endif()
add_library(sibling_part INTERFACE)
add_library(volumetric_kit::sibling_part ALIAS sibling_part)
