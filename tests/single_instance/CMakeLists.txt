# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Configured (never built) by the vkc_single_instance_warning_* tests: a
# consumer whose shared library links the core through a static library, the way
# a sibling's shared library would. With BUILD_SHARED_LIBS=OFF the core is
# static, and the configure must warn.
cmake_minimum_required(VERSION 3.21)
project(vkc_single_instance LANGUAGES CXX)

add_subdirectory("${VKC_SOURCE_DIR}" volumetric_kit_core)

add_library(vkc_single_instance_static STATIC shared.cpp)
target_link_libraries(vkc_single_instance_static
                      PUBLIC volumetric_kit::core_base)

add_library(vkc_single_instance_shared SHARED shared.cpp)
target_link_libraries(vkc_single_instance_shared
                      PRIVATE vkc_single_instance_static)
