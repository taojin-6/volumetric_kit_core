# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The core's version, for a project that added or found it (DECISIONS.md,
# "Consumers pin, and an application declares the core first"). Call these after
# FetchContent_MakeAvailable(volumetric_kit_core) or
# find_package(volumetric_kit_core): they read the VKC_CORE_VERSION that
# volumetric_kit::core_base carries. The core's CMakeLists defines them, and the
# installed package config includes this file.
#
# vkc_require_core(<min-version> [VULKAN] [PACKAGE <package>])
#
# Fails the configure unless the core is <min-version> or newer -- the oldest
# the caller builds with -- and, with VULKAN, has its vulkan tier. In
# <package>'s package config, after find_dependency(volumetric_kit_core),
# PACKAGE <package> refuses as find_dependency does: it sets <package>_FOUND
# FALSE and <package>_NOT_FOUND_MESSAGE and returns from the config, so an
# optional find_package(<package>) leaves the configure running.
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

# vkc_require_core's checks. With PACKAGE, a refusal sets <package>_FOUND,
# <package>_NOT_FOUND_MESSAGE and _vkc_require_core_refused in the caller's
# scope instead of failing the configure.
function(_vkc_require_core)
  list(JOIN ARGV " " _call)
  set(_call "vkc_require_core(${_call})")
  set(min_version "${ARGV0}")
  cmake_parse_arguments(PARSE_ARGV 1 _arg "VULKAN" "PACKAGE" "")
  if(_arg_UNPARSED_ARGUMENTS
     OR _arg_KEYWORDS_MISSING_VALUES
     OR NOT min_version MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "${_call}: use vkc_require_core(<min-version> [VULKAN] "
                        "[PACKAGE <package>]), with a version such as 0.1.0")
  endif()
  set(_who "${PROJECT_NAME}")
  if(DEFINED _arg_PACKAGE)
    if(NOT CMAKE_FIND_PACKAGE_NAME STREQUAL _arg_PACKAGE)
      message(
        FATAL_ERROR
          "${_call}: PACKAGE ${_arg_PACKAGE} is for ${_arg_PACKAGE}'s package "
          "config, as find_package(${_arg_PACKAGE}) reads it")
    endif()
    set(_who "${_arg_PACKAGE}")
  endif()
  _vkc_core_version_of(_found "${_call}")

  get_target_property(_imported volumetric_kit::core_base IMPORTED)
  if(_imported)
    set(_origin "the core installed at ${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(_use_it "and point CMAKE_PREFIX_PATH or volumetric_kit_core_DIR at it")
    # A package config re-found the core itself; elsewhere FetchContent may have
    # found it.
    if(NOT DEFINED _arg_PACKAGE)
      string(APPEND _use_it ", or, where FetchContent found the core, build "
             "the pinned one (FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)")
    endif()
  else()
    get_filename_component(_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
                           DIRECTORY)
    set(_origin "the core at ${_source}")
  endif()

  set(_refusal)
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
    string(CONCAT _refusal "${_who} needs volumetric_kit_core ${min_version} "
                  "or newer, but ${_origin} is ${_found}: ${_fix}.")
  elseif(_arg_VULKAN AND NOT TARGET volumetric_kit::core_vulkan)
    if(_imported)
      set(_fix "install one built with VKC_WITH_VULKAN ON ${_use_it}")
    else()
      string(CONCAT _fix "set VKC_WITH_VULKAN ON before the build's first "
                    "FetchContent_MakeAvailable(volumetric_kit_core)")
    endif()
    string(CONCAT _refusal "${_who} needs volumetric_kit_core's vulkan tier, "
                  "which ${_origin} was built without: ${_fix}.")
  endif()

  if(NOT _refusal)
    return()
  endif()
  if(NOT DEFINED _arg_PACKAGE)
    message(FATAL_ERROR "${_refusal}")
  endif()
  set(${_arg_PACKAGE}_FOUND
      FALSE
      PARENT_SCOPE)
  set(${_arg_PACKAGE}_NOT_FOUND_MESSAGE
      "${_refusal}"
      PARENT_SCOPE)
  set(_vkc_require_core_refused
      TRUE
      PARENT_SCOPE)
endfunction()

# A macro, so that a refusal under PACKAGE returns from the package config, as
# find_dependency's does.
macro(vkc_require_core)
  _vkc_require_core(${ARGV})
  if(_vkc_require_core_refused)
    unset(_vkc_require_core_refused)
    return()
  endif()
endmacro()
