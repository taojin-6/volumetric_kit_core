# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Warns when the build links a static core_base into a shared or module library
# (DECISIONS.md, "One instance per process"). Every binary that links a static
# core_base carries its own copy of the process-global log handler, so the
# application's set_log_handler would reach only one of them -- and nothing else
# would say so. The check runs once every target exists: deferred to the end of
# the top-level directory, whoever's that is. A binary built outside CMake (an
# Xcode framework target, say) is beyond it.

# Sets <out> to TRUE when <target>'s <property> (LINK_LIBRARIES or
# INTERFACE_LINK_LIBRARIES) reaches core_base without crossing a shared library,
# which would carry its own copy and is checked on its own.
function(_vkc_links_core_base target property out)
  set(${out}
      FALSE
      PARENT_SCOPE)
  list(APPEND _vkc_visiting "${target}") # guards against link cycles
  get_target_property(_libs "${target}" ${property})
  if(NOT _libs)
    return()
  endif()
  foreach(_lib IN LISTS _libs)
    # Unwrap the generator expressions CMake itself writes around a static
    # library's private dependencies; skip anything else that is not a target.
    string(REGEX REPLACE "^\\$<(LINK_ONLY|BUILD_INTERFACE):(.*)>$" "\\2" _lib
                         "${_lib}")
    if(NOT TARGET "${_lib}")
      continue()
    endif()
    get_target_property(_aliased "${_lib}" ALIASED_TARGET)
    if(_aliased)
      set(_lib "${_aliased}")
    endif()
    if(_lib STREQUAL "core_base")
      set(${out}
          TRUE
          PARENT_SCOPE)
      return()
    endif()
    get_target_property(_type "${_lib}" TYPE)
    if(_type MATCHES "^(STATIC|OBJECT|INTERFACE)_LIBRARY$" AND NOT _lib IN_LIST
                                                               _vkc_visiting)
      _vkc_links_core_base("${_lib}" INTERFACE_LINK_LIBRARIES _found)
      if(_found)
        set(${out}
            TRUE
            PARENT_SCOPE)
        return()
      endif()
    endif()
  endforeach()
endfunction()

# Sets <out> to every buildsystem target defined in <dir> and below.
function(_vkc_targets_under dir out)
  get_property(
    _targets
    DIRECTORY "${dir}"
    PROPERTY BUILDSYSTEM_TARGETS)
  get_property(
    _subdirs
    DIRECTORY "${dir}"
    PROPERTY SUBDIRECTORIES)
  foreach(_subdir IN LISTS _subdirs)
    _vkc_targets_under("${_subdir}" _sub_targets)
    list(APPEND _targets ${_sub_targets})
  endforeach()
  set(${out}
      "${_targets}"
      PARENT_SCOPE)
endfunction()

function(_vkc_check_single_instance)
  get_target_property(_type core_base TYPE)
  if(NOT _type STREQUAL "STATIC_LIBRARY")
    return()
  endif()
  _vkc_targets_under("${CMAKE_SOURCE_DIR}" _targets)
  set(_shared)
  foreach(_target IN LISTS _targets)
    get_target_property(_target_type "${_target}" TYPE)
    if(_target_type MATCHES "^(SHARED|MODULE)_LIBRARY$")
      _vkc_links_core_base("${_target}" LINK_LIBRARIES _found)
      if(_found)
        list(APPEND _shared "${_target}")
      endif()
    endif()
  endforeach()
  if(_shared)
    list(JOIN _shared ", " _names)
    message(
      WARNING
        "volumetric_kit_core: core_base is a static library, but shared "
        "libraries link it: ${_names}. Each binary that links a static "
        "core_base carries its own log handler, so set_log_handler reaches "
        "only one of them. Build the core shared (BUILD_SHARED_LIBS=ON), or "
        "link it into a single binary. See DECISIONS.md, \"One instance per "
        "process\".")
  endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
               _vkc_check_single_instance)
