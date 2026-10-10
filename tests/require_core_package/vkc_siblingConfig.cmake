# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A sibling's installed package config: it re-finds the core and requires the
# version, and the tier, the consumer (CMakeLists.txt) asks for.
include(CMakeFindDependencyMacro)
find_dependency(volumetric_kit_core CONFIG)
set(_vkc_sibling_tier)
if(VKC_SIBLING_REQUIRE_VULKAN)
  set(_vkc_sibling_tier VULKAN)
endif()
vkc_require_core(${VKC_SIBLING_REQUIRE} ${_vkc_sibling_tier} PACKAGE
                 vkc_sibling)
message(STATUS "vkc_sibling's package config read past vkc_require_core")
