# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# base: Status / Result / VKC_TRY / VKC_ASSIGN, VKC_CHECK, the log sink, and the
# version API. Pure CPU, so these run on every machine. The timeout turns a
# deadlock in the log sink's locking into a failure instead of a hung job.
foreach(_vkc_test base_result base_check base_log base_version
                  base_stage_metrics)
  add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
  target_link_libraries(vkc_${_vkc_test}_test PRIVATE volumetric_kit::core_base
                                                      GTest::gtest_main)
  vkc_target_warnings(vkc_${_vkc_test}_test)
  gtest_discover_tests(
    vkc_${_vkc_test}_test
    PROPERTIES
    TIMEOUT 60)
endforeach()

# camera: the geometry, the lens vocabulary and the rig calibration file. Pure
# CPU, so these run on every machine, and on the -fno-exceptions leg they check
# that the file is parsed without them.
if(VKC_WITH_CAMERA)
  foreach(_vkc_test camera_geometry camera_lens camera_rig_calibration)
    add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
    target_link_libraries(vkc_${_vkc_test}_test
                          PRIVATE volumetric_kit::core_camera GTest::gtest_main)
    vkc_target_warnings(vkc_${_vkc_test}_test)
    gtest_discover_tests(
      vkc_${_vkc_test}_test
      PROPERTIES
      TIMEOUT 60)
  endforeach()
endif()

# vulkan: the VkResult helpers, the format helpers (vulkan_format, and
# vulkan_format_reference against Vulkan-Utility-Libraries, skipped where its
# headers are not installed), UniqueHandle, DeviceRequirements, the queue-family
# reduction and the memory-type masks need no device; the device tests skip
# without one, unless VKC_REQUIRE_VULKAN_DEVICE is set (CI sets it beside
# lavapipe), and fail on a validation error under VKC_TEST_VALIDATION=1
# (tests/vulkan_fixture.hpp).
if(VKC_WITH_VULKAN)
  foreach(
    _vkc_test
    vulkan_result
    vulkan_format
    vulkan_format_reference
    vulkan_unique_handle
    vulkan_requirements
    vulkan_queue_families
    vulkan_memory_types
    vulkan_device
    vulkan_allocator
    vulkan_allocation_budget
    vulkan_resources
    vulkan_compute
    vulkan_command_batch
    vulkan_timer
    vulkan_external_memory
    vulkan_shared_device)
    add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
    target_link_libraries(vkc_${_vkc_test}_test
                          PRIVATE volumetric_kit::core_vulkan GTest::gtest_main)
    vkc_target_warnings(vkc_${_vkc_test}_test)
    gtest_discover_tests(
      vkc_${_vkc_test}_test
      PROPERTIES
      TIMEOUT 120)
  endforeach()

  # The queue-family reduction, the memory-type masks and the queue-plan choice
  # are internal to core_vulkan, so their tests -- and the device tests that
  # check a resource landed in its placement's types -- reach into the source
  # directory for them.
  foreach(
    _vkc_test
    vulkan_queue_families
    vulkan_memory_types
    vulkan_allocator
    vulkan_allocation_budget
    vulkan_compute
    vulkan_shared_device
    vulkan_command_batch)
    target_include_directories(
      vkc_${_vkc_test}_test
      PRIVATE "${PROJECT_SOURCE_DIR}/src/volumetric_kit/core/vulkan")
  endforeach()

  # Exercise budget admission against real VMA suballocations with controlled
  # budget figures, through a VMA allocator of the test's own; no public
  # allocator internals or production budget overrides are exposed to tests. One
  # VMA implementation per link: a static core's comes from its archive, and
  # only a shared core, which hides its own, leaves the test to instantiate one.
  target_include_directories(
    vkc_vulkan_allocation_budget_test SYSTEM
    PRIVATE
      "$<TARGET_PROPERTY:GPUOpen::VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>"
  )
  get_target_property(_vkc_vulkan_type core_vulkan TYPE)
  if(_vkc_vulkan_type STREQUAL "SHARED_LIBRARY")
    target_compile_definitions(vkc_vulkan_allocation_budget_test
                               PRIVATE VKC_TEST_PRIVATE_VMA)
  endif()

  # The compute tests' kernels, compiled and embedded once as a consumer would
  # (cmake/vkc_shaders.cmake), into an INTERFACE library both tests link, so a
  # build with tests needs glslc.
  add_library(vkc_test_shaders INTERFACE)
  vkc_embed_shaders(
    vkc_test_shaders
    SYMBOL_PREFIX
    vkc_test_
    SHADERS
    shaders/add.comp
    shaders/fill.comp
    shaders/batch_visibility.vert
    shaders/batch_visibility.frag)
  foreach(_vkc_test vulkan_compute vulkan_command_batch vulkan_timer)
    target_link_libraries(vkc_${_vkc_test}_test PRIVATE vkc_test_shaders)
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
