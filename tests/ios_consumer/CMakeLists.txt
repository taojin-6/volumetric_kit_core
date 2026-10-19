# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Configured (never built) for iOS by the vkc_ios_* tests: an application that
# adds the core first, then finds Vulkan and glm as recon and gfx do
# (cmake/vkc_ios.cmake). It first writes stubs into its build directory:
# MoltenVK's iOS release at 1.4.1, 1.4.2 and 1.4.3, whose headers say Vulkan
# 1.4.351, 1.4.352 and 1.4.353 (moltenvk-<version>/: the version lines MoltenVK
# and FindVulkan read, and an empty library), one whose header has no version
# (moltenvk-none/), and glm 1.0.3 as Homebrew installs it (brew/: a keg, linked
# into an include/ beside Vulkan headers), which the core's search finds through
# CMAKE_PREFIX_PATH.
cmake_minimum_required(VERSION 3.24)
project(vkc_ios_consumer LANGUAGES CXX)

foreach(_patch 1 2 3)
  set(_root "${CMAKE_BINARY_DIR}/moltenvk-1.4.${_patch}")
  file(WRITE "${_root}/include/MoltenVK/mvk_private_api.h"
       "#define MVK_VERSION_MAJOR   1\n#define MVK_VERSION_MINOR   4\n"
       "#define MVK_VERSION_PATCH   ${_patch}\n")
  file(
    WRITE "${_root}/include/vulkan/vulkan_core.h"
    "#define VK_HEADER_VERSION 35${_patch}\n"
    "#define VK_HEADER_VERSION_COMPLETE "
    "VK_MAKE_API_VERSION(0, 1, 4, VK_HEADER_VERSION)\n")
  file(WRITE "${_root}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" "")
endforeach()
set(_root "${CMAKE_BINARY_DIR}/moltenvk-none")
file(WRITE "${_root}/include/MoltenVK/mvk_private_api.h" "")
file(WRITE "${_root}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" "")
set(_brew "${CMAKE_BINARY_DIR}/brew")
file(REMOVE_RECURSE "${_brew}")
file(WRITE "${_brew}/Cellar/glm/1.0.3/include/glm/glm.hpp" "// glm 1.0.3\n")
file(WRITE "${_brew}/include/vulkan/vulkan.h" "")
file(CREATE_LINK ../Cellar/glm/1.0.3/include/glm "${_brew}/include/glm"
     SYMBOLIC)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/no_glm")
list(PREPEND CMAKE_PREFIX_PATH "${_brew}")

set(VKC_WITH_VULKAN ON)
add_subdirectory("${VKC_SOURCE_DIR}" volumetric_kit_core)
message(STATUS "vkc_ios_consumer: the core is added")

find_package(Vulkan REQUIRED)
message(
  STATUS "vkc_ios_consumer: Vulkan ${Vulkan_VERSION}, ${Vulkan_LIBRARIES}")
get_target_property(_links volumetric_kit::core_vulkan INTERFACE_LINK_LIBRARIES)
list(JOIN _links ", " _links)
message(STATUS "vkc_ios_consumer: core_vulkan links ${_links}")
find_package(glm CONFIG REQUIRED)
# Again, as a subdirectory of a directory that found glm does.
find_package(glm CONFIG REQUIRED)
get_target_property(_glm glm::glm INTERFACE_INCLUDE_DIRECTORIES)
file(
  GLOB _held
  RELATIVE "${_glm}"
  "${_glm}/*")
message(
  STATUS "vkc_ios_consumer: glm::glm's include directory holds [${_held}]")

# An upgrade, as Homebrew makes it: the old keg goes and the link moves to the
# new one. The build's include directory must reach the new glm unchanged.
file(REMOVE_RECURSE "${_brew}/Cellar/glm/1.0.3" "${_brew}/include/glm")
file(WRITE "${_brew}/Cellar/glm/1.0.4/include/glm/glm.hpp" "// glm 1.0.4\n")
file(CREATE_LINK ../Cellar/glm/1.0.4/include/glm "${_brew}/include/glm"
     SYMBOLIC)
file(STRINGS "${_glm}/glm/glm.hpp" _header LIMIT_COUNT 1)
message(STATUS "vkc_ios_consumer: after an upgrade, glm::glm has ${_header}")
