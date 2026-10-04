# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a sibling repo: consumes volumetric_kit_core the two ways the
# family does, and -- as calib, recon and gfx do -- installs and exports its own
# library, which links the core PUBLIC. CI configures it twice:
#
# -DVKC_CONSUME=find_package -DCMAKE_PREFIX_PATH=<install prefix>
# -DVKC_CONSUME=subdirectory -DVKC_SOURCE_DIR=<checkout>
#
# The second mirrors FetchContent: the core is not the top-level project, so its
# tests and -Werror must stay off, while its install rules stay on -- without
# them the export below fails CMake's generate step.
cmake_minimum_required(VERSION 3.21)
project(vkc_package_consumer LANGUAGES CXX)

include(GNUInstallDirs)

set(VKC_CONSUME
    "find_package"
    CACHE STRING "find_package or subdirectory")

if(VKC_CONSUME STREQUAL "find_package")
  find_package(volumetric_kit_core 0.0 REQUIRED CONFIG)
elseif(VKC_CONSUME STREQUAL "subdirectory")
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

add_library(vkc_package_consumer_lib STATIC parse.cpp)
target_link_libraries(vkc_package_consumer_lib PUBLIC volumetric_kit::core)

add_executable(vkc_package_consumer main.cpp)
target_link_libraries(vkc_package_consumer PRIVATE vkc_package_consumer_lib)
# The vulkan tier, when the core has it: an installed core built with it, or a
# subproject configured -DVKC_WITH_VULKAN=ON, as recon and gfx will.
if(TARGET volumetric_kit::core_vulkan)
  target_compile_definitions(vkc_package_consumer
                             PRIVATE VKC_CONSUMER_HAS_VULKAN)
endif()

install(TARGETS vkc_package_consumer_lib EXPORT vkc_package_consumerTargets)
install(
  EXPORT vkc_package_consumerTargets
  NAMESPACE vkc_package_consumer::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vkc_package_consumer")
