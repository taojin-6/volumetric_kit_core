# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The vkc_source_commits tests' project (tests/CMakeLists.txt).
# source_commits_rebuild.cmake builds it in a git checkout of its own: stamp
# prints what the header holds for the project, for the package vkc_sc_dep, a
# second checkout that FETCHCONTENT_SOURCE_DIR_VKC_SC_DEP must name, and for the
# package vkc_sc_local, its folder local/. -DVKC_SC_CALL="<arguments>" makes
# vkc_source_commits(<arguments>) before all that, a misuse that must fail the
# configure (tests/CMakeLists.txt).
cmake_minimum_required(VERSION 3.21)
project(vkc_source_commits_consumer LANGUAGES CXX)

include("${VKC_SOURCE_DIR}/cmake/vkc_source_commits.cmake")
set(VKC_SC_CALL
    ""
    CACHE STRING "A misused call's arguments, or empty")

add_executable(stamp main.cpp)
# One folder for every generator, multi-config ones too.
set_target_properties(stamp PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                       "${CMAKE_BINARY_DIR}/bin$<0:>")

if(NOT VKC_SC_CALL STREQUAL "")
  separate_arguments(_call UNIX_COMMAND "${VKC_SC_CALL}")
  vkc_source_commits(${_call})
endif()

include(FetchContent)
# Never fetched: the override stands in for a sibling's checkout.
FetchContent_Declare(
  vkc_sc_dep GIT_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/no_such_repository")
FetchContent_Declare(vkc_sc_local SOURCE_DIR
                                  "${CMAKE_CURRENT_SOURCE_DIR}/local")
FetchContent_MakeAvailable(vkc_sc_dep vkc_sc_local)

vkc_source_commits(
  stamp
  HEADER
  generated/source_commits.h
  PREFIX
  VKC_SC
  SOURCES
  project=.
  PACKAGES
  dep=vkc_sc_dep
  local=vkc_sc_local)
