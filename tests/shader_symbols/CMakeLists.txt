# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Configured (never built) by the vkc_embed_shaders_prefixes_* tests: two
# libraries each embedding fill.comp, with SYMBOL_PREFIX a_ and
# ${VKC_SECOND_PREFIX}. With a_ twice, both would emit a_fill_comp_spv, and the
# configure must fail.
cmake_minimum_required(VERSION 3.21)
project(vkc_shader_symbols LANGUAGES CXX)

# The shader functions as the installed package config gives them, with the GLSL
# compiler FindVulkan reports.
find_package(Vulkan)
include("${VKC_SOURCE_DIR}/cmake/vkc_shaders.cmake")

set(_shader "${VKC_SOURCE_DIR}/tests/shaders/fill.comp")
add_library(vkc_first_library INTERFACE)
vkc_embed_shaders(vkc_first_library SYMBOL_PREFIX a_ SHADERS "${_shader}")
add_library(vkc_second_library INTERFACE)
vkc_embed_shaders(vkc_second_library SYMBOL_PREFIX ${VKC_SECOND_PREFIX} SHADERS
                  "${_shader}")
