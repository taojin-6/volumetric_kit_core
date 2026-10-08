# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

cmake_minimum_required(VERSION 3.21)

# When VERSION moves, and what takes it: DECISIONS.md, "Consumers pin, and an
# application declares the core first".
project(
  volumetric_kit_core
  VERSION 0.1.0
  DESCRIPTION
    "Shared foundation for the volumetric_kit family: error handling and the Vulkan core"
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
# The vulkan tier needs a Vulkan SDK (headers and loader; MoltenVK on Apple). ON
# by default only at the top level: a sibling that uses it (recon, gfx) sets it
# ON before fetching the core, and one that does not (calib's headless solver)
# then needs no Vulkan installed to build.
option(VKC_WITH_VULKAN "Build the vulkan tier (needs a Vulkan SDK)"
       ${PROJECT_IS_TOP_LEVEL})
# The Vulkan test support a sibling's tests link (tests/support/; README.md,
# "Use it in your project"). This repository's own tests build it too.
option(VKC_BUILD_TEST_SUPPORT "Build the Vulkan test support for a sibling" OFF)
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
# vkc_require_core / vkc_core_version, and with the vulkan tier
# vkc_compile_shaders / vkc_embed_shaders: functions are global in CMake, so a
# project that fetches the core can call them once it is added.
include(vkc_require_core)
if(VKC_WITH_VULKAN)
  include(vkc_shaders)
  # The oldest Vulkan headers the tier supports (DECISIONS.md, "Vulkan headers
  # come from the system"), checked at configure where FindVulkan reports a
  # version: by the build (src/volumetric_kit/core/vulkan/) and by the installed
  # package's find_dependency. vulkan.hpp checks the same as it compiles; keep
  # the two in step.
  if(APPLE)
    set(VKC_VULKAN_MIN_VERSION 1.3.208)
  else()
    set(VKC_VULKAN_MIN_VERSION 1.3.204)
  endif()
endif()

# --- Dependencies + library tiers --------------------------------------------
add_subdirectory(third_party)
add_subdirectory(src)

if(VKC_BUILD_TEST_SUPPORT AND NOT VKC_WITH_VULKAN)
  message(FATAL_ERROR "VKC_BUILD_TEST_SUPPORT needs VKC_WITH_VULKAN")
endif()
if(VKC_WITH_VULKAN AND (VKC_BUILD_TEST_SUPPORT OR VKC_BUILD_TESTS))
  add_subdirectory(tests/support)
endif()

if(VKC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
