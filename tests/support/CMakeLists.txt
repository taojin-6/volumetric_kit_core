# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# core_test_main: GoogleTest's main, which a suite links in place of
# GTest::gtest_main so that on Linux its death tests' children dump no core
# (test_main.cpp). It needs no Vulkan, so the base tier's tests link it too.
add_library(core_test_main STATIC EXCLUDE_FROM_ALL test_main.cpp)
add_library(volumetric_kit::core_test_main ALIAS core_test_main)
target_link_libraries(core_test_main
                      PUBLIC $<TARGET_NAME_IF_EXISTS:GTest::gtest>)
vkc_target_warnings(core_test_main)

if(NOT VKC_WITH_VULKAN)
  return()
endif()

# core_test_policy: the environment's policy, the two-family device and the
# image write, needing no test framework (vulkan_policy.hpp,
# two_family_device.hpp, write_image.hpp). core_test_support: the GoogleTest
# fixtures on them (vulkan_fixture.hpp), linking the consumer's GTest::gtest,
# which a consumer without googletest need not provide. Each is built only for a
# target that links it, and static, so the shared instances and devices are the
# test binary's own.
add_library(core_test_policy STATIC EXCLUDE_FROM_ALL
            vulkan_policy.cpp two_family_device.cpp write_image.cpp)
add_library(volumetric_kit::core_test_policy ALIAS core_test_policy)
target_include_directories(core_test_policy
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(core_test_policy PUBLIC volumetric_kit::core_vulkan)
# A ValidationSession switches on the ring-output poison, internal to
# core_vulkan (ring_slot.hpp).
target_include_directories(
  core_test_policy
  PRIVATE "${PROJECT_SOURCE_DIR}/src/volumetric_kit/core/vulkan")
vkc_target_warnings(core_test_policy)

add_library(core_test_support STATIC EXCLUDE_FROM_ALL vulkan_fixture.cpp)
add_library(volumetric_kit::core_test_support ALIAS core_test_support)
target_link_libraries(
  core_test_support PUBLIC core_test_policy
                           $<TARGET_NAME_IF_EXISTS:GTest::gtest>)
vkc_target_warnings(core_test_support)
