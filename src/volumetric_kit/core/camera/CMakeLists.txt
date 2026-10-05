# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# --- core_camera: cameras and the rig calibration file -----------------------
# Double-precision geometry, the lens vocabulary (image size, pinhole
# intrinsics, OpenCV's rational distortion) and the rig calibration file that
# calib writes and a capture reads (DECISIONS.md, "The camera tier"). Pure CPU
# on top of the base tier; nlohmann/json parses the file, privately.
include(GenerateExportHeader)
include(GNUInstallDirs)

set(_vkc_generated_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

add_library(core_camera)
add_library(volumetric_kit::core_camera ALIAS core_camera)

target_sources(core_camera PRIVATE geometry.cpp lens.cpp rig_calibration.cpp)

# nlohmann/json is private: only rig_calibration.cpp includes it, never a public
# header, so it is read as a SYSTEM include directory (third_party/) and never
# enters core_camera's exported interface.
if(NOT VKC_JSON_INCLUDE_DIR)
  message(FATAL_ERROR "core_camera: VKC_JSON_INCLUDE_DIR is unset -- "
                      "third_party/ must make nlohmann/json available first")
endif()
target_include_directories(core_camera SYSTEM PRIVATE "${VKC_JSON_INCLUDE_DIR}")

generate_export_header(
  core_camera
  BASE_NAME
  VKC_CAMERA
  EXPORT_MACRO_NAME
  VKC_CAMERA_API
  EXPORT_FILE_NAME
  "${_vkc_generated_inc}/volumetric_kit/core/camera/export.hpp")

# As core_base: a prefixed file name for a shared lib directory, and a
# MAJOR.MINOR soname before 1.0.
#
# TODO: from 1.0, SOVERSION ${PROJECT_VERSION_MAJOR} alone.
set_target_properties(
  core_camera
  PROPERTIES OUTPUT_NAME volumetric_kit_core_camera
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON)

get_target_property(_vkc_type core_camera TYPE)
if(_vkc_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(core_camera PUBLIC VKC_CAMERA_STATIC_DEFINE)
endif()

target_compile_features(core_camera PUBLIC cxx_std_17)

target_include_directories(
  core_camera
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${_vkc_generated_inc}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# PUBLIC: the public headers return Status / Result.
target_link_libraries(core_camera PUBLIC core_base)

vkc_target_warnings(core_camera)
vkc_propagate_sanitizers(core_camera)

if(VKC_INSTALL)
  install(
    TARGETS core_camera
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
