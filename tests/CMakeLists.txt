# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# base: Status / Result / VKC_TRY / VKC_ASSIGN, VKC_CHECK, the log sink, and the
# version API. Pure CPU, so these run on every machine. The timeout turns a
# deadlock in the log sink's locking into a failure instead of a hung job.
foreach(_vkc_test base_result base_check base_log base_version)
  add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
  target_link_libraries(vkc_${_vkc_test}_test PRIVATE volumetric_kit::core_base
                                                      GTest::gtest_main)
  vkc_target_warnings(vkc_${_vkc_test}_test)
  gtest_discover_tests(
    vkc_${_vkc_test}_test
    PROPERTIES
    TIMEOUT 60)
endforeach()

# vulkan: the VkResult helpers, UniqueHandle, DeviceRequirements and the
# queue-family reduction need no device; the device tests skip without one,
# unless VKC_REQUIRE_VULKAN_DEVICE is set (CI sets it beside lavapipe), and fail
# on a validation error under VKC_TEST_VALIDATION=1 (tests/vulkan_fixture.hpp).
if(VKC_WITH_VULKAN)
  foreach(
    _vkc_test
    vulkan_result
    vulkan_unique_handle
    vulkan_requirements
    vulkan_queue_families
    vulkan_device
    vulkan_allocator
    vulkan_resources
    vulkan_compute
    vulkan_command_batch)
    add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
    target_link_libraries(vkc_${_vkc_test}_test
                          PRIVATE volumetric_kit::core_vulkan GTest::gtest_main)
    vkc_target_warnings(vkc_${_vkc_test}_test)
    gtest_discover_tests(
      vkc_${_vkc_test}_test
      PROPERTIES
      TIMEOUT 120)
  endforeach()

  # The queue-family reduction is internal to core_vulkan, so its test reaches
  # into the source directory for it.
  target_include_directories(
    vkc_vulkan_queue_families_test
    PRIVATE "${PROJECT_SOURCE_DIR}/src/volumetric_kit/core/vulkan")

  # The compute tests' kernels, compiled and embedded as a consumer would
  # (cmake/vkc_shaders.cmake), so a build with tests needs glslc.
  foreach(_vkc_test vulkan_compute vulkan_command_batch)
    vkc_embed_shaders(vkc_${_vkc_test}_test SYMBOL_PREFIX vkc_test_ SHADERS
                      shaders/add.comp shaders/fill.comp)
  endforeach()
endif()

# The one-instance warning (cmake/vkc_single_instance.cmake): configure a
# consumer that links a static core into a shared library and expect the
# warning, then the same with the core shared and expect none. Configure only;
# nothing is built.
foreach(_vkc_shared OFF ON)
  set(_vkc_name vkc_single_instance_warning_shared_${_vkc_shared})
  add_test(
    NAME ${_vkc_name}
    COMMAND
      "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/single_instance" -B
      "${CMAKE_CURRENT_BINARY_DIR}/${_vkc_name}" -G "${CMAKE_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DVKC_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DBUILD_SHARED_LIBS=${_vkc_shared}")
  if(_vkc_shared)
    set_tests_properties(${_vkc_name} PROPERTIES FAIL_REGULAR_EXPRESSION
                                                 "core_base is a static")
  else()
    set_tests_properties(${_vkc_name} PROPERTIES PASS_REGULAR_EXPRESSION
                                                 "core_base is a static")
  endif()
endforeach()

# package_consumer/ is not added here: CI configures it as a separate project
# against an installed copy (find_package) and against the source tree
# (add_subdirectory), the two ways the sibling repos consume this one.
