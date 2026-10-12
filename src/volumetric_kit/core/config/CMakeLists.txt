# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
include(GenerateExportHeader)
include(GNUInstallDirs)
set(_vkc_generated_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
add_library(core_config config.cpp)
add_library(volumetric_kit::core_config ALIAS core_config)
generate_export_header(
  core_config
  BASE_NAME
  VKC_CONFIG
  EXPORT_MACRO_NAME
  VKC_CONFIG_API
  EXPORT_FILE_NAME
  "${_vkc_generated_inc}/volumetric_kit/core/config/export.hpp")
set_target_properties(
  core_config
  PROPERTIES OUTPUT_NAME volumetric_kit_core_config
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON)
get_target_property(_vkc_type core_config TYPE)
if(_vkc_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(core_config PUBLIC VKC_CONFIG_STATIC_DEFINE)
endif()
target_compile_features(core_config PUBLIC cxx_std_17)
target_include_directories(
  core_config
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${_vkc_generated_inc}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(core_config SYSTEM PRIVATE "${VKC_JSON_INCLUDE_DIR}")
target_link_libraries(core_config PUBLIC core_base)
vkc_target_warnings(core_config)
vkc_propagate_sanitizers(core_config)
if(VKC_INSTALL)
  install(
    TARGETS core_config
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
