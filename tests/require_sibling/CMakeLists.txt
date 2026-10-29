# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Configured (never built) by the vkc_require_sibling_* tests: adds a stand-in
# sibling, volumetric_kit_sibling 0.3.0 (sibling/), as FetchContent adds one,
# then makes the call a project that builds on it makes, with the umbrella,
# floor and targets given (-DVKC_REQUIRE_UMBRELLA, -DVKC_REQUIRE_MIN,
# -DVKC_REQUIRE_TARGETS). -DVKC_SIBLING_UNVERSIONED=ON stands in for a sibling
# whose umbrella carries no version.
cmake_minimum_required(VERSION 3.21)
project(vkc_sibling_consumer LANGUAGES NONE)

set(VKC_REQUIRE_UMBRELLA
    "volumetric_kit::sibling"
    CACHE STRING "The umbrella target to require")
set(VKC_REQUIRE_MIN
    "0.3.0"
    CACHE STRING "The oldest sibling this consumer builds with")
set(VKC_REQUIRE_TARGETS
    "volumetric_kit::sibling_part"
    CACHE STRING "The sibling's targets this consumer needs")

include("${VKC_SOURCE_DIR}/cmake/vkc_require_sibling.cmake")
include(FetchContent)
FetchContent_Declare(volumetric_kit_sibling
                     SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sibling")
FetchContent_MakeAvailable(volumetric_kit_sibling)

vkc_require_sibling(${VKC_REQUIRE_UMBRELLA} ${VKC_REQUIRE_MIN} TARGETS
                    ${VKC_REQUIRE_TARGETS})
message(STATUS "vkc_require_sibling accepted volumetric_kit_sibling")
