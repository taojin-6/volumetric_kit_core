# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# base: Status / Result / VKC_TRY / VKC_ASSIGN, VKC_CHECK, the log sink, and the
# version API. Pure CPU, so these run on every machine.
foreach(_vkc_test base_result base_check base_log base_version)
  add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
  target_link_libraries(vkc_${_vkc_test}_test PRIVATE volumetric_kit::core_base
                                                      GTest::gtest_main)
  vkc_target_warnings(vkc_${_vkc_test}_test)
  gtest_discover_tests(vkc_${_vkc_test}_test)
endforeach()

# package_consumer/ is not added here: CI configures it as a separate project
# against an installed copy (find_package) and against the source tree
# (add_subdirectory), the two ways the sibling repos consume this one.
