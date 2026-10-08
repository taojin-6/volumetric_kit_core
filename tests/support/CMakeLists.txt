# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# --- core_test_support: the Vulkan test fixture the family links ------------
# The GoogleTest fixtures every sibling's Vulkan tests build on
# (include/volumetric_kit/core/testing/vulkan_fixture.hpp). Built for this
# repository's tests, and for a sibling that sets VKC_BUILD_TEST_SUPPORT ON
# before fetching the core. It links the googletest the build provides
# (GTest::gtest: this repository's, or the sibling's), and is neither installed
# nor exported: an installed core has no test support. Static, so its shared
# instances and devices are the test binary's own.
add_library(core_test_support STATIC vulkan_fixture.cpp)
add_library(volumetric_kit::core_test_support ALIAS core_test_support)
target_include_directories(core_test_support
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(core_test_support PUBLIC volumetric_kit::core_vulkan
                                               GTest::gtest)
vkc_target_warnings(core_test_support)
