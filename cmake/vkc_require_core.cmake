# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# vkc_require_core(<min-version>)
#
# Fails the configure unless the volumetric_kit_core this project links is
# <min-version> or newer. A sibling calls it with the oldest release it builds
# with, after FetchContent_MakeAvailable or find_package: its own pin yields to
# a core an application declared first, and FetchContent may find an installed
# core instead, so this checks what arrived rather than what was asked for
# (DECISIONS.md, "Consumers pin, and an application declares the core first").
# The version is the one volumetric_kit::core_base carries (VKC_CORE_VERSION),
# whether the core was built here or installed.
#
# Defined by the core's own CMakeLists, so a project that fetches the core can
# call it, and by the installed package config. A core older than 0.1.0 defines
# neither: CMake then stops at the call as an unknown command.
function(vkc_require_core min_version)
  if(NOT min_version MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(
      FATAL_ERROR
        "vkc_require_core(${min_version}): not a version, such as 0.1.0")
  endif()
  if(NOT TARGET volumetric_kit::core_base)
    message(
      FATAL_ERROR
        "vkc_require_core(${min_version}): no volumetric_kit::core_base here; "
        "call it after FetchContent_MakeAvailable(volumetric_kit_core) or "
        "find_package(volumetric_kit_core)")
  endif()

  get_target_property(_found volumetric_kit::core_base VKC_CORE_VERSION)
  if(NOT _found)
    set(_found "older than 0.1.0") # which carried no version
  elseif(NOT _found VERSION_LESS min_version)
    return()
  endif()

  get_target_property(_imported volumetric_kit::core_base IMPORTED)
  if(_imported)
    set(_origin "the core installed at ${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    string(
      CONCAT _fix "install volumetric_kit_core ${min_version} or newer "
             "and point CMAKE_PREFIX_PATH or volumetric_kit_core_DIR at it")
  else()
    get_filename_component(_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
                           DIRECTORY)
    set(_origin "the core at ${_source}")
    string(
      CONCAT
        _fix
        "pin v${min_version} or newer in the build's first "
        "FetchContent_Declare(volumetric_kit_core), which FetchContent "
        "keeps over every later one, or point "
        "FETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_CORE at a checkout that new")
  endif()
  message(
    FATAL_ERROR
      "${PROJECT_NAME} needs volumetric_kit_core ${min_version} or newer, but "
      "${_origin} is ${_found}: ${_fix}.")
endfunction()
