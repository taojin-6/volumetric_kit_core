# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The core's version, for a project that added or found it (DECISIONS.md,
# "Consumers pin, and an application declares the core first"). Call these after
# FetchContent_MakeAvailable(volumetric_kit_core) or
# find_package(volumetric_kit_core): they read the VKC_CORE_VERSION that
# volumetric_kit::core_base carries. The core's CMakeLists defines them, and the
# installed package config includes this file.
#
# vkc_require_core(<min-version> [VULKAN])
#
# Fails the configure unless the core is <min-version> or newer -- the oldest
# the caller builds with -- and, with VULKAN, has its vulkan tier.
#
# vkc_core_version(<out> [MAJOR_MINOR])
#
# Sets <out> to the core's version, or with MAJOR_MINOR to its MAJOR.MINOR.

# Sets <out> to the version volumetric_kit::core_base carries (NOTFOUND on a
# core older than 0.1.0), or fails the configure in <caller> without the core.
function(_vkc_core_version_of out caller)
  if(NOT TARGET volumetric_kit::core_base)
    message(
      FATAL_ERROR
        "${caller}: no volumetric_kit::core_base here; call it after "
        "FetchContent_MakeAvailable(volumetric_kit_core) or "
        "find_package(volumetric_kit_core)")
  endif()
  get_target_property(_version volumetric_kit::core_base VKC_CORE_VERSION)
  set(${out}
      "${_version}"
      PARENT_SCOPE)
endfunction()

function(vkc_core_version out)
  list(JOIN ARGV " " _call)
  set(_call "vkc_core_version(${_call})")
  cmake_parse_arguments(PARSE_ARGV 1 _arg "MAJOR_MINOR" "" "")
  if(_arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "${_call}: use vkc_core_version(<out> [MAJOR_MINOR])")
  endif()
  _vkc_core_version_of(_version "${_call}")
  if(NOT _version)
    message(
      FATAL_ERROR "${_call}: volumetric_kit::core_base carries no version")
  endif()
  if(_arg_MAJOR_MINOR)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" _version "${_version}")
  endif()
  set(${out}
      "${_version}"
      PARENT_SCOPE)
endfunction()

function(vkc_require_core min_version)
  list(JOIN ARGV " " _call)
  set(_call "vkc_require_core(${_call})")
  cmake_parse_arguments(PARSE_ARGV 1 _arg "VULKAN" "" "")
  if(_arg_UNPARSED_ARGUMENTS OR NOT min_version MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(
      FATAL_ERROR
        "${_call}: use vkc_require_core(<min-version> [VULKAN]), with a "
        "version such as 0.1.0")
  endif()
  _vkc_core_version_of(_found "${_call}")

  get_target_property(_imported volumetric_kit::core_base IMPORTED)
  if(_imported)
    set(_origin "the core installed at ${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    string(
      CONCAT _use_it
             "and point CMAKE_PREFIX_PATH or volumetric_kit_core_DIR at "
             "it, or, where FetchContent found the core, build the pinned one "
             "(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)")
  else()
    get_filename_component(_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
                           DIRECTORY)
    set(_origin "the core at ${_source}")
  endif()

  if(NOT _found OR _found VERSION_LESS min_version)
    if(NOT _found)
      set(_found "older than 0.1.0")
    endif()
    if(_imported)
      set(_fix "install volumetric_kit_core ${min_version} or newer ${_use_it}")
    else()
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
        "${PROJECT_NAME} needs volumetric_kit_core ${min_version} or newer, "
        "but ${_origin} is ${_found}: ${_fix}.")
  endif()

  if(_arg_VULKAN AND NOT TARGET volumetric_kit::core_vulkan)
    if(_imported)
      set(_fix "install one built with VKC_WITH_VULKAN ON ${_use_it}")
    else()
      string(CONCAT _fix "set VKC_WITH_VULKAN ON before the build's first "
                    "FetchContent_MakeAvailable(volumetric_kit_core)")
    endif()
    message(
      FATAL_ERROR
        "${PROJECT_NAME} needs volumetric_kit_core's vulkan tier, which "
        "${_origin} was built without: ${_fix}.")
  endif()
endfunction()
