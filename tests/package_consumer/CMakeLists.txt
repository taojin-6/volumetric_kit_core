# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a sibling repo: consumes volumetric_kit_core the two ways the
# family does. CI configures it twice:
#
# -DVKC_CONSUME=find_package -DCMAKE_PREFIX_PATH=<install prefix>
# -DVKC_CONSUME=subdirectory -DVKC_SOURCE_DIR=<checkout>
#
# The second mirrors FetchContent: the core is not the top-level project, so its
# tests, install rules and -Werror must stay off.
cmake_minimum_required(VERSION 3.21)
project(vkc_package_consumer LANGUAGES CXX)

set(VKC_CONSUME
    "find_package"
    CACHE STRING "find_package or subdirectory")

if(VKC_CONSUME STREQUAL "find_package")
  find_package(volumetric_kit_core 0.0 REQUIRED CONFIG)
elseif(VKC_CONSUME STREQUAL "subdirectory")
  add_subdirectory("${VKC_SOURCE_DIR}" volumetric_kit_core)
  foreach(_option VKC_BUILD_TESTS VKC_INSTALL VKC_WARNINGS_AS_ERRORS)
    if(${_option})
      message(
        FATAL_ERROR "${_option} must default OFF for a non-top-level core")
    endif()
  endforeach()
else()
  message(FATAL_ERROR "VKC_CONSUME must be find_package or subdirectory")
endif()

add_executable(vkc_package_consumer main.cpp)
target_link_libraries(vkc_package_consumer PRIVATE volumetric_kit::core)
