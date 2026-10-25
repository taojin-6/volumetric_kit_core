# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# What an iOS build of the family needs, in one place (DECISIONS.md, "iOS links
# MoltenVK statically, set up by the core"). The root CMakeLists includes it in
# the vulkan tier's block when CMAKE_SYSTEM_NAME is iOS, before src/ and so
# before the tier's find_package(Vulkan); in an application that declares the
# core first, that is also before every recon and gfx find of Vulkan and glm.
# CMake's own iOS support does the rest, with no toolchain file:
#
# cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
# -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 [-DMoltenVK_ROOT=<dir>]
#
# Reads:
#
# * MoltenVK_ROOT: the MoltenVK/ directory of MoltenVK's iOS release (it holds
#   include/ and static/), at VKC_MOLTENVK_MIN_VERSION or newer. Required: from
#   -D, which the build remembers, else from the environment, else the root the
#   last configure took.
# * VKC_GLM_INCLUDE_DIR: the directory holding glm/glm.hpp. Found among the
#   host's installed packages (Homebrew's glm on a Mac) unless set.
#
# Sets:
#
# * Vulkan_LIBRARY and Vulkan_INCLUDE_DIR: MoltenVK's static library and
#   headers, FORCEd. Every find_package(Vulkan) then makes Vulkan::Vulkan from
#   them.
# * VKC_MOLTENVK_FRAMEWORKS: the system frameworks the static library needs at
#   an app's link; core_vulkan links them PUBLIC.
# * glm::glm: the answer to every find_package(glm CONFIG), the host's glm
#   headers, header-only, on a directory of this build that holds only a link to
#   the glm/ found.
if(CMAKE_VERSION VERSION_LESS 3.24)
  # CMAKE_FIND_PACKAGE_REDIRECTS_DIR, for glm.
  message(
    FATAL_ERROR
      "An iOS build needs CMake 3.24 or newer; this is ${CMAKE_VERSION}.")
endif()

set(VKC_MOLTENVK_MIN_VERSION 1.4.2)

# Typed, so that a relative -D is taken against the directory cmake runs in.
set(MoltenVK_ROOT
    ""
    CACHE PATH "The MoltenVK/ directory of MoltenVK's iOS release")
if(NOT MoltenVK_ROOT)
  set(MoltenVK_ROOT "$ENV{MoltenVK_ROOT}")
endif()
if(NOT MoltenVK_ROOT)
  set(MoltenVK_ROOT "${_VKC_MOLTENVK_ROOT}")
endif()
set(_vkc_mvk_lib
    "${MoltenVK_ROOT}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a")
if(NOT MoltenVK_ROOT OR NOT EXISTS "${_vkc_mvk_lib}")
  if(MoltenVK_ROOT)
    set(_vkc_missing "Not found: ${_vkc_mvk_lib}")
  else()
    set(_vkc_missing "MoltenVK_ROOT is not set.")
  endif()
  message(
    FATAL_ERROR
      "An iOS build links MoltenVK's iOS release statically, a prerequisite "
      "installed once. Point -DMoltenVK_ROOT, or the MoltenVK_ROOT environment "
      "variable, at the MoltenVK/ directory of MoltenVK-ios.tar "
      "${VKC_MOLTENVK_MIN_VERSION} or newer "
      "(https://github.com/KhronosGroup/MoltenVK/releases). ${_vkc_missing}")
endif()
set(_vkc_mvk_header "${MoltenVK_ROOT}/include/MoltenVK/mvk_private_api.h")
file(STRINGS "${_vkc_mvk_header}" _vkc_mvk_defs
     REGEX "^#define MVK_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+")
set(_vkc_mvk_version "")
foreach(_vkc_part MAJOR MINOR PATCH)
  string(REGEX MATCH "MVK_VERSION_${_vkc_part} +([0-9]+)" _vkc_match
               "${_vkc_mvk_defs}")
  list(APPEND _vkc_mvk_version "${CMAKE_MATCH_1}")
endforeach()
list(JOIN _vkc_mvk_version "." _vkc_mvk_version)
if(NOT _vkc_mvk_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "No MoltenVK version in ${_vkc_mvk_header}.")
endif()
if(_vkc_mvk_version VERSION_LESS VKC_MOLTENVK_MIN_VERSION)
  message(
    FATAL_ERROR
      "MoltenVK ${_vkc_mvk_version} at ${MoltenVK_ROOT} is older than "
      "${VKC_MOLTENVK_MIN_VERSION}, the oldest an iOS build takes.")
endif()
message(STATUS "MoltenVK ${_vkc_mvk_version} (iOS, static): ${MoltenVK_ROOT}")
set(_VKC_MOLTENVK_ROOT
    "${MoltenVK_ROOT}"
    CACHE INTERNAL "The MoltenVK_ROOT the last iOS configure took")
set(Vulkan_LIBRARY
    "${_vkc_mvk_lib}"
    CACHE FILEPATH "MoltenVK's iOS static library" FORCE)
set(Vulkan_INCLUDE_DIR
    "${MoltenVK_ROOT}/include"
    CACHE PATH "MoltenVK's Vulkan headers" FORCE)
# MoltenVK_Runtime_UserGuide.md, "Install as static library".
set(VKC_MOLTENVK_FRAMEWORKS
    "-framework Metal" "-framework Foundation" "-framework QuartzCore"
    "-framework CoreGraphics" "-framework IOSurface" "-framework UIKit")

# glm. A missing one is reported by the find_package(glm) that needs it, so a
# build without glm (the core's alone) configures.
find_path(VKC_GLM_INCLUDE_DIR glm/glm.hpp NO_CMAKE_FIND_ROOT_PATH
          DOC "The directory holding glm/glm.hpp, for an iOS build's glm::glm")
set(_vkc_glm_config "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/glm-config.cmake")
if(EXISTS "${VKC_GLM_INCLUDE_DIR}/glm/glm.hpp")
  set(_vkc_glm_include "${PROJECT_BINARY_DIR}/vkc_ios/include")
  file(MAKE_DIRECTORY "${_vkc_glm_include}")
  file(CREATE_LINK "${VKC_GLM_INCLUDE_DIR}/glm" "${_vkc_glm_include}/glm"
       SYMBOLIC)
  message(STATUS "glm (iOS, header-only): ${VKC_GLM_INCLUDE_DIR}/glm")
  file(
    WRITE "${_vkc_glm_config}"
    "# Written by volumetric_kit_core's cmake/vkc_ios.cmake.\n"
    "if(NOT TARGET glm::glm)\n"
    "  add_library(glm::glm INTERFACE IMPORTED)\n"
    "  set_target_properties(glm::glm PROPERTIES\n"
    "    INTERFACE_INCLUDE_DIRECTORIES \"${_vkc_glm_include}\")\n"
    "endif()\n")
else()
  file(
    WRITE "${_vkc_glm_config}"
    "# Written by volumetric_kit_core's cmake/vkc_ios.cmake.\n"
    "set(glm_FOUND FALSE)\n"
    "set(glm_NOT_FOUND_MESSAGE \"an iOS build takes glm's headers from the "
    "host, and none were found: install glm (brew install glm), or point "
    "-DVKC_GLM_INCLUDE_DIR at the directory holding glm/glm.hpp\")\n")
endif()
