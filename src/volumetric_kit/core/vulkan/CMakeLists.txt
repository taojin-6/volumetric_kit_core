# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# --- core_vulkan: the shared Vulkan core -------------------------------------
# The instance, physical-device selection against a library's requirements, and
# the logical device every sibling creates or adopts -- the foundation the
# allocator, compute and shared-device pieces build on (DECISIONS.md, "The
# vulkan tier"). Vulkan comes from the system: the headers and loader of a
# Vulkan SDK, MoltenVK on Apple.
include(GenerateExportHeader)
include(GNUInstallDirs)

find_package(Vulkan)
if(NOT Vulkan_FOUND)
  message(
    FATAL_ERROR
      "VKC_WITH_VULKAN is ON but no Vulkan SDK was found. Install one "
      "(macOS: brew install vulkan-headers vulkan-loader molten-vk; Ubuntu: "
      "apt install libvulkan-dev), or configure with -DVKC_WITH_VULKAN=OFF.")
endif()

set(_vkc_generated_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

add_library(core_vulkan)
add_library(volumetric_kit::core_vulkan ALIAS core_vulkan)

target_sources(
  core_vulkan
  PRIVATE allocator.cpp
          buffer.cpp
          command_batch.cpp
          command_buffer.cpp
          command_pool.cpp
          compute_kernel.cpp
          compute_pipeline.cpp
          compute_util.cpp
          descriptor.cpp
          gpu_timer.cpp
          device.cpp
          device_requirements.cpp
          external_memory.cpp
          image.cpp
          instance.cpp
          physical_device_info.cpp
          query_pool.cpp
          queue_families.hpp
          shader.cpp
          support.hpp
          sync.cpp
          vk_result.cpp)

# VMA is private: only allocator.cpp includes it, never a public header. Its
# include directory is read from the target VMA's own build defines, whether
# FetchContent built it or a dependency provider found it (third_party/), and
# SYSTEM keeps its warnings out of the -Werror surface. It is read, not linked,
# so VMA never enters core_vulkan's exported interface.
if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
  message(FATAL_ERROR "core_vulkan: no GPUOpen::VulkanMemoryAllocator target "
                      "-- third_party/ must make VMA available before src/")
endif()
target_include_directories(
  core_vulkan SYSTEM
  PRIVATE
    "$<TARGET_PROPERTY:GPUOpen::VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>"
)

generate_export_header(
  core_vulkan
  BASE_NAME
  VKC_VULKAN
  EXPORT_MACRO_NAME
  VKC_VULKAN_API
  EXPORT_FILE_NAME
  "${_vkc_generated_inc}/volumetric_kit/core/vulkan/export.hpp")

# As core_base: a prefixed file name for a shared lib directory, and a
# MAJOR.MINOR soname before 1.0.
#
# TODO: from 1.0, SOVERSION ${PROJECT_VERSION_MAJOR} alone.
set_target_properties(
  core_vulkan
  PROPERTIES OUTPUT_NAME volumetric_kit_core_vulkan
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON)

get_target_property(_vkc_type core_vulkan TYPE)
if(_vkc_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(core_vulkan PUBLIC VKC_VULKAN_STATIC_DEFINE)
endif()

target_compile_features(core_vulkan PUBLIC cxx_std_17)

target_include_directories(
  core_vulkan
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${_vkc_generated_inc}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# PUBLIC both: the public headers name Status / Result and Vulkan types, and
# consumers call vk* themselves. The package config re-finds Vulkan.
target_link_libraries(core_vulkan PUBLIC core_base Vulkan::Vulkan)

vkc_target_warnings(core_vulkan)
vkc_propagate_sanitizers(core_vulkan)

if(VKC_INSTALL)
  install(
    TARGETS core_vulkan
    EXPORT volumetric_kit_coreTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(
    DIRECTORY "${_vkc_generated_inc}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING
    PATTERN "*.hpp")
  # The shader functions, which the package config includes.
  install(FILES "${PROJECT_SOURCE_DIR}/cmake/vkc_shaders.cmake"
                "${PROJECT_SOURCE_DIR}/cmake/vkc_embed_spirv.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/volumetric_kit_core")
endif()
