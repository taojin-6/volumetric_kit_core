# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a sibling repo: consumes volumetric_kit_core the two ways the
# family does, and -- as calib, recon and gfx do -- installs and exports its own
# library, which links the core PUBLIC, and builds GPU tests on the core's test
# support. CI configures it twice:
#
# -DVKC_CONSUME=find_package -DCMAKE_PREFIX_PATH=<install prefix>
# -DVKC_CONSUME=subdirectory -DVKC_SOURCE_DIR=<checkout>
#
# The second mirrors FetchContent: the core is not the top-level project, so its
# tests and -Werror must stay off, while its install rules stay on -- without
# them the export below fails CMake's generate step. -DVKC_CONSUMER_GTEST=OFF
# stands in for a sibling without googletest, and -DVKC_CONSUMER_REQUIRE (or
# -DVKC_CONSUMER_REQUIRE_VULKAN=ON) for one needing a newer core (or its vulkan
# tier).
cmake_minimum_required(VERSION 3.21)
project(vkc_package_consumer LANGUAGES CXX)

include(GNUInstallDirs)

set(VKC_CONSUME
    "find_package"
    CACHE STRING "find_package or subdirectory")
option(VKC_CONSUMER_GTEST "Provide googletest, and a GPU test on the fixtures"
       ON)
set(VKC_CONSUMER_REQUIRE
    "0.1.0"
    CACHE STRING "The oldest volumetric_kit_core this consumer builds with")
option(VKC_CONSUMER_REQUIRE_VULKAN "Require the core's vulkan tier" OFF)

if(VKC_CONSUME STREQUAL "find_package")
  find_package(volumetric_kit_core REQUIRED CONFIG)
  foreach(_target core_test_policy core_test_support)
    if(TARGET volumetric_kit::${_target})
      message(FATAL_ERROR "an installed core must not export ${_target}")
    endif()
  endforeach()
elseif(VKC_CONSUME STREQUAL "subdirectory")
  # A sibling with GPU tests asks for the core's test support before adding it.
  if(VKC_WITH_VULKAN)
    set(VKC_BUILD_TEST_SUPPORT ON)
  endif()
  add_subdirectory("${VKC_SOURCE_DIR}" volumetric_kit_core)
  foreach(_option VKC_BUILD_TESTS VKC_WARNINGS_AS_ERRORS)
    if(${_option})
      message(
        FATAL_ERROR "${_option} must default OFF for a non-top-level core")
    endif()
  endforeach()
  if(NOT VKC_INSTALL)
    message(FATAL_ERROR "VKC_INSTALL must default ON for a non-top-level core")
  endif()
else()
  message(FATAL_ERROR "VKC_CONSUME must be find_package or subdirectory")
endif()

# As a sibling checks the core it got, whichever way it came, and reads the
# MAJOR.MINOR its package config would re-find the core at.
set(_vkc_tier)
if(VKC_CONSUMER_REQUIRE_VULKAN)
  set(_vkc_tier VULKAN)
endif()
vkc_require_core(${VKC_CONSUMER_REQUIRE} ${_vkc_tier})
vkc_core_version(_vkc_version)
vkc_core_version(_vkc_major_minor MAJOR_MINOR)
message(
  STATUS "volumetric_kit_core ${_vkc_version} (MAJOR.MINOR ${_vkc_major_minor})"
)

# One version throughout: the one vkc_require_core read is an installed core's
# package version file's and soname's, and (main.cpp) the headers' and the
# linked library's.
get_target_property(_vkc_imported volumetric_kit::core_base IMPORTED)
if(_vkc_imported)
  if(NOT volumetric_kit_core_VERSION STREQUAL _vkc_version)
    message(
      FATAL_ERROR "the package version file says ${volumetric_kit_core_VERSION}"
                  ", core_base ${_vkc_version}")
  endif()
  get_target_property(_vkc_type volumetric_kit::core_base TYPE)
  if(_vkc_type STREQUAL "SHARED_LIBRARY")
    # Before 1.0 the soname carries MAJOR.MINOR.
    get_target_property(_vkc_configs volumetric_kit::core_base
                        IMPORTED_CONFIGURATIONS)
    list(GET _vkc_configs 0 _vkc_config)
    get_target_property(_vkc_soname volumetric_kit::core_base
                        IMPORTED_SONAME_${_vkc_config})
    string(REPLACE "." "\\." _vkc_pattern "${_vkc_major_minor}")
    if(NOT _vkc_soname MATCHES "\\.${_vkc_pattern}(\\.|$)")
      message(FATAL_ERROR "core_base's soname, ${_vkc_soname}, is not version "
                          "${_vkc_major_minor}")
    endif()
  endif()
endif()

add_library(vkc_package_consumer_lib STATIC parse.cpp)
target_link_libraries(vkc_package_consumer_lib PUBLIC volumetric_kit::core)

add_executable(vkc_package_consumer main.cpp)
target_link_libraries(vkc_package_consumer PRIVATE vkc_package_consumer_lib)
target_compile_definitions(
  vkc_package_consumer PRIVATE "VKC_CONSUMER_CORE_VERSION=\"${_vkc_version}\"")
# The vulkan tier, when the core has it: an installed core built with it, or a
# subproject configured -DVKC_WITH_VULKAN=ON, as recon and gfx will.
if(TARGET volumetric_kit::core_vulkan)
  target_compile_definitions(vkc_package_consumer
                             PRIVATE VKC_CONSUMER_HAS_VULKAN)
  # The shader functions, as a sibling embeds its kernels: from the installed
  # package config, or defined by the core's own CMakeLists.
  if(NOT COMMAND vkc_embed_shaders)
    message(FATAL_ERROR "the vulkan tier brought no vkc_embed_shaders")
  endif()
  vkc_embed_shaders(vkc_package_consumer SYMBOL_PREFIX vkc_consumer_ SHADERS
                    smoke.comp)
endif()

# GPU tests on the core's test support, never installed: a main() of its own on
# the policy alone, and a GoogleTest one on the fixtures, linking the googletest
# the consumer provides -- fetched after the core, as the fixtures' link to
# GTest::gtest resolves when the build is generated.
if(TARGET volumetric_kit::core_test_policy)
  add_executable(vkc_package_consumer_policy_test policy_test.cpp)
  target_link_libraries(vkc_package_consumer_policy_test
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
  add_executable(vkc_package_consumer_gpu_test gpu_test.cpp)
  target_link_libraries(
    vkc_package_consumer_gpu_test PRIVATE volumetric_kit::core_test_support
                                          GTest::gtest_main)
endif()

install(TARGETS vkc_package_consumer_lib EXPORT vkc_package_consumerTargets)
install(
  EXPORT vkc_package_consumerTargets
  NAMESPACE vkc_package_consumer::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vkc_package_consumer")
