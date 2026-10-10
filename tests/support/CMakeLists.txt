# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# core_test_policy: the environment's policy, needing no test framework
# (vulkan_policy.hpp). core_test_support: the GoogleTest fixtures on it
# (vulkan_fixture.hpp), linking the consumer's GTest::gtest, which a consumer
# without googletest need not provide. Each is built only for a target that
# links it, and static, so the shared instances and devices are the test
# binary's own.
add_library(core_test_policy STATIC EXCLUDE_FROM_ALL vulkan_policy.cpp)
add_library(volumetric_kit::core_test_policy ALIAS core_test_policy)
target_include_directories(core_test_policy
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(core_test_policy PUBLIC volumetric_kit::core_vulkan)
vkc_target_warnings(core_test_policy)

add_library(core_test_support STATIC EXCLUDE_FROM_ALL vulkan_fixture.cpp)
add_library(volumetric_kit::core_test_support ALIAS core_test_support)
target_link_libraries(
  core_test_support PUBLIC core_test_policy
                           $<TARGET_NAME_IF_EXISTS:GTest::gtest>)
vkc_target_warnings(core_test_support)
