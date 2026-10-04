# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

cmake_minimum_required(VERSION 3.21)

project(
  volumetric_kit_core
  VERSION 0.0.1
  DESCRIPTION
    "Shared foundation for the volumetric_kit family: error handling, and (staged) the Vulkan core, camera models and sensors"
  HOMEPAGE_URL "https://github.com/taojin-6/volumetric_kit_core"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Options -----------------------------------------------------------------
# Tests and -Werror default ON only when this is the top-level project, so a
# consumer that pulls it in via FetchContent builds the library tiers alone,
# with no warnings-as-errors from its newer compiler.
option(VKC_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(VKC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors"
       ${PROJECT_IS_TOP_LEVEL})
# Install rules default ON everywhere. A sibling that installs and exports its
# own targets links the core PUBLIC, so its export references the core's
# targets: CMake refuses to generate unless they are in an export set of their
# own, and the sibling's installed package needs the core installed beside it. A
# consumer that installs nothing never runs these rules.
option(VKC_INSTALL "Generate install/export rules" ON)
# Lint first-party targets with clang-tidy as they compile (.clang-tidy); see
# cmake/vkc_clang_tidy.cmake. Off by default: it slows the build.
option(VKC_CLANG_TIDY "Run clang-tidy on first-party targets" OFF)

# Semicolon list of sanitizers (e.g. "address;undefined"); empty = off. Applied
# globally -- see cmake/vkc_sanitizers.cmake for why.
set(VKC_SANITIZE
    ""
    CACHE STRING "Sanitizers to enable, e.g. address;undefined (empty = off)")

include(vkc_warnings)
include(vkc_sanitizers)
include(vkc_clang_tidy)
include(vkc_single_instance)

# --- Dependencies + library tiers --------------------------------------------
add_subdirectory(third_party)
add_subdirectory(src)

if(VKC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
