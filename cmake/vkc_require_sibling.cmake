# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A sibling's version, for a project that added it (DECISIONS.md, the 2026-10-28
# entry on floors). cmake/vkc_require_core.cmake includes this file, so the
# core's CMakeLists defines it with vkc_require_core.
#
# vkc_require_sibling(<umbrella-target> <min-version> [TARGETS <target>...])
#
# Call it after FetchContent_MakeAvailable(<package>). Fails the configure
# unless <umbrella-target> exists, the VKC_PACKAGE_VERSION it carries is
# <min-version> or newer -- the oldest the caller builds with -- and each listed
# target exists. The package volumetric_kit_<name> has the umbrella
# volumetric_kit::<name>, and sets VKC_PACKAGE_VERSION on it to its
# project(VERSION).

function(vkc_require_sibling)
  list(JOIN ARGV " " _call)
  set(_call "vkc_require_sibling(${_call})")
  string(CONCAT _usage "${_call}: use vkc_require_sibling(<umbrella-target> "
                "<min-version> [TARGETS <target>...]), with a version such as "
                "0.1.0")
  if(ARGC LESS 2)
    message(FATAL_ERROR "${_usage}")
  endif()
  set(umbrella "${ARGV0}")
  set(min_version "${ARGV1}")
  cmake_parse_arguments(PARSE_ARGV 2 _arg "" "" "TARGETS")
  if(_arg_UNPARSED_ARGUMENTS
     OR _arg_KEYWORDS_MISSING_VALUES
     OR NOT min_version MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "${_usage}")
  endif()

  string(REPLACE "::" "_" _package "${umbrella}")
  if(NOT TARGET "${umbrella}")
    message(FATAL_ERROR "${_call}: no ${umbrella} here; call it after "
                        "FetchContent_MakeAvailable(${_package})")
  endif()
  # project() caches <package>_SOURCE_DIR, and FetchContent sets it too.
  string(REGEX REPLACE "^.*::" "" _name "${umbrella}")
  if(${_package}_SOURCE_DIR)
    set(_origin "the ${_name} at ${${_package}_SOURCE_DIR}")
  else()
    set(_origin "the ${_name} this build added")
  endif()

  get_target_property(_found "${umbrella}" VKC_PACKAGE_VERSION)
  _vkc_require_floor("${_package}" "${_origin}" "${_found}" "${min_version}")
  foreach(_target IN LISTS _arg_TARGETS)
    if(NOT TARGET "${_target}")
      message(
        FATAL_ERROR
          "${PROJECT_NAME} needs ${_target}, which ${_origin} was built "
          "without: set the option that builds it before the build's first "
          "FetchContent_MakeAvailable(${_package}).")
    endif()
  endforeach()
endfunction()

# _vkc_require_floor(<package> <origin> <found> <min-version>)
#
# Internal to vkc_require_sibling and vkc_require_core: fails the configure
# unless <found>, the version of the <package> that <origin> names, is
# <min-version> or newer, naming both and how to move the pin. An empty or
# NOTFOUND <found> is older than every floor.
function(_vkc_require_floor package origin found min_version)
  if(found STREQUAL "" OR found MATCHES "-NOTFOUND$")
    set(found "unversioned")
  elseif(NOT found VERSION_LESS min_version)
    return()
  endif()
  string(TOUPPER "${package}" _upper)
  string(
    CONCAT
      _refusal
      "${PROJECT_NAME} needs ${package} ${min_version} or newer, but ${origin} "
      "is ${found}: pin v${min_version} or newer in the build's first "
      "FetchContent_Declare(${package}), which FetchContent keeps over every "
      "later one, or point FETCHCONTENT_SOURCE_DIR_${_upper} at a checkout "
      "that new.")
  message(FATAL_ERROR "${_refusal}")
endfunction()
