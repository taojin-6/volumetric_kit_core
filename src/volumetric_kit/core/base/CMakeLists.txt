# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# --- core_base: the bottom tier ----------------------------------------------
# Exception-free Status/Result error handling, the VKC_CHECK contract check, the
# pluggable log sink, and the version API. Pure C++17 with no dependencies, so
# every other tier, and every sibling repo, can build on it -- including a
# headless or device-less consumer.
include(GenerateExportHeader)
include(GNUInstallDirs)

set(_vkc_generated_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

add_library(core_base)
add_library(volumetric_kit::core_base ALIAS core_base)

target_sources(core_base PRIVATE check.cpp log.cpp log_internal.hpp result.cpp
                                 version.cpp)

# Generated headers: the per-tier VKC_BASE_API export macro (each tier generates
# its own, so their macros and guards never collide) and the configured version
# header.
generate_export_header(
  core_base
  BASE_NAME
  VKC_BASE
  EXPORT_MACRO_NAME
  VKC_BASE_API
  EXPORT_FILE_NAME
  "${_vkc_generated_inc}/volumetric_kit/core/base/export.hpp")
configure_file(
  "${PROJECT_SOURCE_DIR}/include/volumetric_kit/core/version.hpp.in"
  "${_vkc_generated_inc}/volumetric_kit/core/version.hpp" @ONLY)

# OUTPUT_NAME: the file lands in a shared lib directory, where a bare
# `libcore_base` would say nothing about whose it is. SOVERSION: before 1.0 a
# minor release may break the ABI (the package version file accepts only the
# same minor), so the soname carries MAJOR.MINOR and two releases installed to
# one prefix are told apart by the loader rather than by a crash.
#
# TODO: from 1.0, SOVERSION ${PROJECT_VERSION_MAJOR} alone.
set_target_properties(
  core_base
  PROPERTIES OUTPUT_NAME volumetric_kit_core_base
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON)

# With a static archive, VKC_BASE_API must expand to nothing for consumers.
get_target_property(_vkc_type core_base TYPE)
if(_vkc_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(core_base PUBLIC VKC_BASE_STATIC_DEFINE)
endif()

target_compile_features(core_base PUBLIC cxx_std_17)

target_include_directories(
  core_base
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${_vkc_generated_inc}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

vkc_target_warnings(core_base)
vkc_propagate_sanitizers(core_base)

if(VKC_INSTALL)
  install(
    TARGETS core_base
    EXPORT volumetric_kit_coreTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(
    DIRECTORY "${_vkc_generated_inc}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING
    PATTERN "*.hpp")
endif()
