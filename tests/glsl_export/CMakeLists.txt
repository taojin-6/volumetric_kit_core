# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Run by the vkc_export_glsl_* tests: a package's target exports
# shaders/scale.glsl but not shaders/internal.glsl beside it (vkc_export_glsl),
# and a consumer's kernel, shaders/scaled.comp, includes the export through
# INCLUDE_TARGETS, embedded and compiled. The configure checks that the export
# root holds what it exported alone, and vkc_export_glsl_build builds both
# (vkc_export_glsl_rebuild, ../glsl_export_rebuild.cmake, builds a copy again).
# -DVKC_GLSL_EXPORT_INTERNAL=ON exports internal.glsl too, in a second call, so
# a configure without it after one with it drops it from the root. These must
# fail the configure: a -DVKC_GLSL_INCLUDE naming a target that exports nothing
# (vkc_glsl_unexported) or no target at all; -DVKC_GLSL_EXPORT_AGAIN=ON,
# exporting scale.glsl's include path from a second target; and a
# -DVKC_GLSL_PREFIX that is absolute or climbs out of the root.
cmake_minimum_required(VERSION 3.21)
project(vkc_glsl_export LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VKC_GLSL_INCLUDE
    "vkc_glsl_exporter"
    CACHE STRING "The target the kernels' INCLUDE_TARGETS names")
set(VKC_GLSL_PREFIX
    "vkc_test/shaders"
    CACHE STRING "The include prefix scale.glsl is exported under")
option(VKC_GLSL_EXPORT_INTERNAL "Export internal.glsl too" OFF)
option(VKC_GLSL_EXPORT_AGAIN "Export scale.glsl from a second target" OFF)

# The shader functions alone, with the GLSL compiler FindVulkan reports.
find_package(Vulkan)
include("${VKC_SOURCE_DIR}/cmake/vkc_shaders.cmake")

add_library(vkc_glsl_exporter INTERFACE)
vkc_export_glsl(vkc_glsl_exporter PREFIX ${VKC_GLSL_PREFIX} FILES
                shaders/scale.glsl)
set(_expected "vkc_test/shaders/scale.glsl")
if(VKC_GLSL_EXPORT_INTERNAL)
  vkc_export_glsl(vkc_glsl_exporter PREFIX vkc_test/shaders FILES
                  shaders/internal.glsl)
  set(_expected "vkc_test/shaders/internal.glsl;${_expected}")
endif()
if(VKC_GLSL_EXPORT_AGAIN)
  add_library(vkc_glsl_second INTERFACE)
  vkc_export_glsl(vkc_glsl_second PREFIX vkc_test/shaders FILES
                  shaders/scale.glsl)
endif()
add_library(vkc_glsl_unexported INTERFACE)

# Once the directory is configured, after vkc_export_glsl's own deferred step:
# the root holds this configure's exports, and none an earlier one made alone.
function(_vkc_check_root)
  get_target_property(_root vkc_glsl_exporter VKC_SHADER_INCLUDE_DIRS)
  file(
    GLOB_RECURSE _held
    LIST_DIRECTORIES false
    RELATIVE "${_root}"
    "${_root}/*")
  if(NOT _held STREQUAL _expected)
    message(FATAL_ERROR "the export root '${_root}' holds '${_held}', not the "
                        "exported '${_expected}' alone")
  endif()
endfunction()
cmake_language(DEFER CALL _vkc_check_root)

add_library(vkc_glsl_consumer STATIC consumer.cpp)
vkc_embed_shaders(
  vkc_glsl_consumer
  SYMBOL_PREFIX
  vkc_glsl_
  INCLUDE_TARGETS
  ${VKC_GLSL_INCLUDE}
  SHADERS
  shaders/scaled.comp)
add_custom_target(vkc_glsl_compiled ALL)
vkc_compile_shaders(vkc_glsl_compiled INCLUDE_TARGETS ${VKC_GLSL_INCLUDE}
                    SHADERS shaders/scaled.comp)
