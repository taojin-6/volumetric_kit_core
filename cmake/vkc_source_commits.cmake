# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# What a build was built from: each source checkout's commit, and whether it had
# changes, read at every build (DECISIONS.md, the 2026-11-03 provenance entry).
# The core's CMakeLists includes this file, so a project that adds the core can
# call vkc_source_commits; the target that call adds runs this same file as a
# script before each build.
#
# vkc_source_commits(<target> HEADER <header> PREFIX <prefix> [SOURCES
# <name>=<dir>...] [PACKAGES <name>=<package>...])
#
# Before every build of <target>, writes <header>, a path relative to the
# current binary directory, and rewrites it only when its text changes, so an
# unchanged checkout recompiles nothing. <header>'s directory is a private
# include directory of <target>. For each source <name>, upper-cased as <NAME>,
# the header defines:
#
# * <prefix>_<NAME>_SHA: "<commit>", the commit checked out in <dir>, or ""
#   where <dir> is not a git checkout's top (an unpacked copy, or a folder
#   inside another checkout) or the build found no git.
# * <prefix>_<NAME>_DIRTY: 1 when git status lists anything there, a tracked
#   file changed or an untracked file that is not ignored; else 0. An ignored
#   file never counts. It is 0 where SHA is "".
# * <prefix>_<NAME>_OVERRIDE, for PACKAGES alone: "<dir>", the
#   FETCHCONTENT_SOURCE_DIR_<PACKAGE> the build was configured with, or "" where
#   none was set.
#
# A SOURCES <dir> is relative to the current source directory. A PACKAGES
# <package> is one FetchContent has added, read from its SOURCE_DIR. Each caller
# formats these values as its own records need.

# Sets <sha_out> and <dirty_out> for the checkout whose top is <dir>, read with
# <git>: as the header's SHA and DIRTY.
function(_vkc_source_commit dir git sha_out dirty_out)
  set(_sha "")
  set(_dirty 0)
  if(git AND IS_DIRECTORY "${dir}")
    execute_process(
      COMMAND "${git}" -C "${dir}" rev-parse --show-toplevel HEAD
      OUTPUT_VARIABLE _lines
      RESULT_VARIABLE _failed
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REPLACE "\n" ";" _lines "${_lines}")
    list(LENGTH _lines _count)
    if(NOT _failed AND _count EQUAL 2)
      list(GET _lines 0 _top)
      list(GET _lines 1 _head)
      # A folder inside some other checkout is not one itself.
      file(REAL_PATH "${dir}" _dir)
      file(REAL_PATH "${_top}" _top)
      if(_dir STREQUAL _top)
        set(_sha "${_head}")
        # The dirty rule, an open decision (DECISIONS.md): untracked files count
        # and ignored ones never do. With --untracked-files=no, tracked changes
        # alone would. No optional lock, so a build never holds the index
        # another git command wants.
        execute_process(
          COMMAND "${git}" --no-optional-locks -C "${dir}" status --porcelain
                  --untracked-files=normal
          OUTPUT_VARIABLE _changes
          RESULT_VARIABLE _failed
          ERROR_QUIET)
        if(_failed OR NOT "${_changes}" STREQUAL "")
          set(_dirty 1)
        endif()
      endif()
    endif()
  endif()
  set(${sha_out}
      "${_sha}"
      PARENT_SCOPE)
  set(${dirty_out}
      "${_dirty}"
      PARENT_SCOPE)
endfunction()

# _vkc_stamp_source(<name> <dir> [<override>])
#
# Internal to the script run below: appends the source's lines to _vkc_text. The
# inputs file vkc_source_commits writes calls it once per source, with an
# override, empty or not, for a package.
function(_vkc_stamp_source name dir)
  _vkc_source_commit("${dir}" "${_vkc_git}" _sha _dirty)
  string(TOUPPER "${name}" _macro)
  set(_macro "${_vkc_prefix}_${_macro}")
  string(APPEND _vkc_text "\n// ${name}\n#define ${_macro}_SHA \"${_sha}\"\n"
         "#define ${_macro}_DIRTY ${_dirty}\n")
  if(ARGC GREATER 2)
    string(REPLACE "\\" "\\\\" _override "${ARGV2}")
    string(REPLACE "\"" "\\\"" _override "${_override}")
    string(APPEND _vkc_text "#define ${_macro}_OVERRIDE \"${_override}\"\n")
  endif()
  set(_vkc_text
      "${_vkc_text}"
      PARENT_SCOPE)
endfunction()

function(vkc_source_commits target)
  list(JOIN ARGV " " _call)
  set(_call "vkc_source_commits(${_call})")
  string(
    CONCAT _usage
           "${_call}: use vkc_source_commits(<target> HEADER <header> PREFIX "
           "<prefix> [SOURCES <name>=<dir>...] [PACKAGES <name>=<package>...]),"
           " with at least one source, and a prefix and names that are C "
           "identifiers")
  cmake_parse_arguments(PARSE_ARGV 1 _arg "" "HEADER;PREFIX" "SOURCES;PACKAGES")
  # Quoted: an absent keyword leaves its variable undefined, which if() would
  # read as the variable's name.
  if(_arg_UNPARSED_ARGUMENTS
     OR _arg_KEYWORDS_MISSING_VALUES
     OR "${_arg_HEADER}" STREQUAL ""
     OR NOT "${_arg_PREFIX}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$"
     OR "${_arg_SOURCES}${_arg_PACKAGES}" STREQUAL "")
    message(FATAL_ERROR "${_usage}")
  endif()
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "${_call}: no target ${target} here; call it after "
                        "the add_library or add_executable that makes it")
  endif()

  find_package(Git QUIET)
  get_filename_component(_header "${_arg_HEADER}" ABSOLUTE BASE_DIR
                         "${CMAKE_CURRENT_BINARY_DIR}")
  string(
    CONCAT _inputs
           "# Written by ${_call} at configure.\n"
           "set(_vkc_header [==[${_header}]==])\n"
           "set(_vkc_prefix ${_arg_PREFIX})\n"
           "set(_vkc_git [==[${GIT_EXECUTABLE}]==])\n")
  set(_names)
  foreach(_kind SOURCES PACKAGES)
    foreach(_entry IN LISTS _arg_${_kind})
      if(NOT _entry MATCHES "^([A-Za-z][A-Za-z0-9_]*)=(.+)$")
        message(FATAL_ERROR "${_usage}; \"${_entry}\" is none")
      endif()
      set(_name "${CMAKE_MATCH_1}")
      set(_value "${CMAKE_MATCH_2}")
      string(TOUPPER "${_name}" _upper)
      if(_upper IN_LIST _names)
        message(FATAL_ERROR "${_call}: two sources are named ${_upper}, which "
                            "names their macros; give each its own name")
      endif()
      list(APPEND _names "${_upper}")
      if(_kind STREQUAL "SOURCES")
        get_filename_component(_dir "${_value}" ABSOLUTE BASE_DIR
                               "${CMAKE_CURRENT_SOURCE_DIR}")
        string(APPEND _inputs "_vkc_stamp_source(${_name} [==[${_dir}]==])\n")
      else()
        include(FetchContent)
        FetchContent_GetProperties(
          "${_value}"
          SOURCE_DIR _dir
          POPULATED _populated)
        if(NOT _populated)
          message(FATAL_ERROR "${_call}: no ${_value} here; call it after "
                              "FetchContent_MakeAvailable(${_value})")
        endif()
        string(TOUPPER "${_value}" _package)
        set(_override "${FETCHCONTENT_SOURCE_DIR_${_package}}")
        string(APPEND _inputs "_vkc_stamp_source(${_name} [==[${_dir}]==] "
               "[==[${_override}]==])\n")
      endif()
    endforeach()
  endforeach()

  set(_inputs_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_source_commits.cmake")
  file(WRITE "${_inputs_file}" "${_inputs}")
  set(_command "${CMAKE_COMMAND}" "-DVKC_SOURCE_COMMITS_INPUTS=${_inputs_file}"
               -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
  # Once now, so the header exists as the build is generated, then before every
  # build of the target. BYPRODUCTS lets Ninja see an unchanged header as one.
  execute_process(COMMAND ${_command} RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${_call}: writing ${_header} failed (${_rc})")
  endif()
  add_custom_target(
    "${target}_source_commits"
    COMMAND ${_command}
    BYPRODUCTS "${_header}"
    VERBATIM)
  add_dependencies("${target}" "${target}_source_commits")
  get_filename_component(_include "${_header}" DIRECTORY)
  target_include_directories("${target}" PRIVATE "${_include}")
endfunction()

# The stamp itself, as the target vkc_source_commits added runs it: cmake
# -DVKC_SOURCE_COMMITS_INPUTS=<file> -P <this file>.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEFINED VKC_SOURCE_COMMITS_INPUTS)
    message(FATAL_ERROR "vkc_source_commits.cmake runs as the target "
                        "vkc_source_commits adds, with its inputs file")
  endif()
  # A build a git hook runs inherits GIT_DIR, GIT_INDEX_FILE and the like
  # (githooks(5)), with which git -C would read the hook's repository for every
  # directory. Cleared as git clears them to run in a submodule: the variables
  # git rev-parse --local-env-vars lists, but for the command line's settings
  # (GIT_CONFIG_PARAMETERS, GIT_CONFIG_COUNT), which still apply.
  foreach(
    _vkc_var IN
    ITEMS GIT_ALTERNATE_OBJECT_DIRECTORIES
          GIT_COMMON_DIR
          GIT_CONFIG
          GIT_DIR
          GIT_GRAFT_FILE
          GIT_IMPLICIT_WORK_TREE
          GIT_INDEX_FILE
          GIT_NO_REPLACE_OBJECTS
          GIT_OBJECT_DIRECTORY
          GIT_PREFIX
          GIT_REPLACE_REF_BASE
          GIT_SHALLOW_FILE
          GIT_WORK_TREE)
    unset(ENV{${_vkc_var}})
  endforeach()
  string(
    CONCAT _vkc_text
           "// Written by volumetric_kit_core's cmake/vkc_source_commits.cmake "
           "at every build;\n// do not edit.\n#pragma once\n")
  include("${VKC_SOURCE_COMMITS_INPUTS}")
  if(EXISTS "${_vkc_header}")
    file(READ "${_vkc_header}" _vkc_old)
    if(_vkc_old STREQUAL _vkc_text)
      return()
    endif()
  endif()
  file(WRITE "${_vkc_header}" "${_vkc_text}")
endif()
