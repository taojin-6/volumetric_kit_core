# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a sibling repo: adds volumetric_kit_core from a checkout as
# FetchContent does (-DVKC_SOURCE_DIR=<checkout>), builds a library that links
# it PUBLIC, as in every sibling, and tests on the core's test support: GPU
# tests, and one on its Vulkan-free part, which a core without the vulkan tier
# still has. The core is not the top-level project, so its tests and -Werror
# must stay off. -DVKC_CONSUMER_GTEST=OFF stands in for a sibling without
# googletest, and -DVKC_CONSUMER_REQUIRE (or -DVKC_CONSUMER_REQUIRE_VULKAN=ON or
# -DVKC_CONSUMER_REQUIRE_CONFIG=ON) for one needing a newer core (or a tier).
cmake_minimum_required(VERSION 3.21)
project(vkc_consumer LANGUAGES CXX)

option(VKC_CONSUMER_GTEST "Provide googletest, and a GPU test on the fixtures"
       ON)
set(VKC_CONSUMER_REQUIRE
    "0.29.0"
    CACHE STRING "The oldest volumetric_kit_core this consumer builds with")
option(VKC_CONSUMER_REQUIRE_CONFIG "Require the config tier" OFF)
option(VKC_CONSUMER_REQUIRE_VULKAN "Require the core's vulkan tier" OFF)

# A sibling with tests asks for the core's test support before adding it;
# without the vulkan tier it gets the Vulkan-free part.
set(VKC_BUILD_TEST_SUPPORT ON)
add_subdirectory("${VKC_SOURCE_DIR}" volumetric_kit_core)
foreach(_option VKC_BUILD_TESTS VKC_WARNINGS_AS_ERRORS)
  if(${_option})
    message(FATAL_ERROR "${_option} must default OFF for a non-top-level core")
  endif()
endforeach()

# As a sibling checks the core it got.
set(_vkc_tier)
if(VKC_CONSUMER_REQUIRE_VULKAN)
  set(_vkc_tier VULKAN)
endif()
if(VKC_CONSUMER_REQUIRE_CONFIG)
  list(APPEND _vkc_tier CONFIG)
endif()
vkc_require_core(${VKC_CONSUMER_REQUIRE} ${_vkc_tier})
vkc_core_version(_vkc_version)
vkc_core_version(_vkc_major_minor MAJOR_MINOR)
message(
  STATUS "volumetric_kit_core ${_vkc_version} (MAJOR.MINOR ${_vkc_major_minor})"
)

add_library(vkc_consumer_lib STATIC parse.cpp)
target_link_libraries(vkc_consumer_lib PUBLIC volumetric_kit::core)

add_executable(vkc_consumer main.cpp)
target_link_libraries(vkc_consumer PRIVATE vkc_consumer_lib)
target_compile_definitions(
  vkc_consumer PRIVATE "VKC_CONSUMER_CORE_VERSION=\"${_vkc_version}\"")
if(TARGET volumetric_kit::core_config)
  target_compile_definitions(vkc_consumer PRIVATE VKC_CONSUMER_HAS_CONFIG)
endif()
# The vulkan tier, when the core has it: configured -DVKC_WITH_VULKAN=ON, as
# recon and gfx do.
if(TARGET volumetric_kit::core_vulkan)
  target_compile_definitions(vkc_consumer PRIVATE VKC_CONSUMER_HAS_VULKAN)
  # The shader functions, as a sibling embeds its kernels, defined by the core's
  # own CMakeLists.
  if(NOT COMMAND vkc_embed_shaders)
    message(FATAL_ERROR "the vulkan tier brought no vkc_embed_shaders")
  endif()
  vkc_embed_shaders(vkc_consumer SYMBOL_PREFIX vkc_consumer_ SHADERS smoke.comp)
endif()

# A test that is a main() of its own on the Vulkan-free support, with or without
# the vulkan tier: CHECK, a scratch directory and the allocation failure.
add_executable(vkc_consumer_check_test check_test.cpp)
target_link_libraries(
  vkc_consumer_check_test PRIVATE volumetric_kit::core_test_base
                                  volumetric_kit::core_test_allocation_failure)

# GPU tests on the core's test support: a main() of its own on the policy alone,
# and a GoogleTest one on the fixtures and the core's test main, linking the
# googletest the consumer provides -- fetched after the core, as their link to
# GTest::gtest resolves when the build is generated.
if(TARGET volumetric_kit::core_test_policy)
  add_executable(vkc_consumer_policy_test policy_test.cpp)
  target_link_libraries(vkc_consumer_policy_test
                        PRIVATE volumetric_kit::core_test_policy)
endif()
if(VKC_CONSUMER_GTEST AND TARGET volumetric_kit::core_test_support)
  include(FetchContent)
  set(BUILD_GMOCK OFF)
  set(INSTALL_GTEST OFF)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
  add_executable(vkc_consumer_gpu_test gpu_test.cpp)
  target_link_libraries(
    vkc_consumer_gpu_test PRIVATE volumetric_kit::core_test_support
                                  volumetric_kit::core_test_main)
endif()
