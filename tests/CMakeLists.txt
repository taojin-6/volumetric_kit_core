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

# vulkan: the VkResult helpers, the format helpers (vulkan_format, and
# vulkan_format_reference against Vulkan-Utility-Libraries, skipped where its
# headers are not installed), UniqueHandle, DeviceRequirements, the queue-family
# reduction and the memory-type masks need no device; the device tests run on
# core_test_support's fixtures (support/), which skip without a device and fail
# on a validation error as the environment asks.
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
    vulkan_shared_device
    testing_vulkan_fixture)
    add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
    target_link_libraries(
      vkc_${_vkc_test}_test PRIVATE volumetric_kit::core_test_support
                                    GTest::gtest_main)
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
  foreach(_vkc_test vulkan_compute vulkan_command_batch vulkan_timer
                    testing_vulkan_fixture)
    target_link_libraries(vkc_${_vkc_test}_test PRIVATE vkc_test_shaders)
  endforeach()

  # core_test_policy alone, in a test that is a main() of its own: no
  # googletest, and a skip CTest reads from the exit code.
  add_executable(vkc_testing_vulkan_policy_test testing_vulkan_policy_test.cpp)
  target_link_libraries(vkc_testing_vulkan_policy_test
                        PRIVATE volumetric_kit::core_test_policy)
  vkc_target_warnings(vkc_testing_vulkan_policy_test)
  add_test(NAME vkc_testing_vulkan_policy_test
           COMMAND vkc_testing_vulkan_policy_test)
  # The skip code is test::kSkipExitCode.
  set_tests_properties(vkc_testing_vulkan_policy_test
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

# core_test_support's fixture in processes of their own, each entry with the
# environment it probes, read from the output: a run without a device fails
# under VKC_REQUIRE_VULKAN_DEVICE and skips without it; one whose layer cannot
# be found fails under VKC_TEST_VALIDATION; two tests in one process share the
# device; a buffer a test never destroys fails the run as the shared device goes
# (LeakSanitizer off: the leak is the point); a fixture chain stops where its
# middle SetUp failed; and under VKC_TEST_SYNC_VALIDATION an instance a test
# makes itself runs synchronization validation.
if(VKC_WITH_VULKAN)
  add_executable(vkc_testing_vulkan_fixture_probe
                 testing_vulkan_fixture_probe.cpp)
  target_link_libraries(
    vkc_testing_vulkan_fixture_probe PRIVATE volumetric_kit::core_test_support
                                             GTest::gtest_main)
  vkc_target_warnings(vkc_testing_vulkan_fixture_probe)

  set(_vkc_probe "$<TARGET_FILE:vkc_testing_vulkan_fixture_probe>")
  set(_vkc_no_driver "${CMAKE_CURRENT_BINARY_DIR}/no_such_driver.json")
  set(_vkc_no_layers "${CMAKE_CURRENT_BINARY_DIR}/no_layers")
  file(MAKE_DIRECTORY "${_vkc_no_layers}")
  set(_vkc_without_driver
      --unset=VKC_TEST_VALIDATION --unset=VKC_TEST_SYNC_VALIDATION
      "VK_DRIVER_FILES=${_vkc_no_driver}" "VK_ICD_FILENAMES=${_vkc_no_driver}")
  set(_vkc_skipped "\\[  SKIPPED \\]")

  add_test(
    NAME vkc_testing_requires_a_device
    COMMAND
      "${CMAKE_COMMAND}" -E env ${_vkc_without_driver}
      VKC_REQUIRE_VULKAN_DEVICE=1 "${_vkc_probe}"
      --gtest_filter=Probe.GetsADevice)
  set_tests_properties(
    vkc_testing_requires_a_device PROPERTIES PASS_REGULAR_EXPRESSION
                                             "VKC_REQUIRE_VULKAN_DEVICE is set")

  add_test(
    NAME vkc_testing_skips_without_a_device
    COMMAND
      "${CMAKE_COMMAND}" -E env ${_vkc_without_driver}
      --unset=VKC_REQUIRE_VULKAN_DEVICE "${_vkc_probe}"
      --gtest_filter=Probe.GetsADevice)
  set_tests_properties(
    vkc_testing_skips_without_a_device
    PROPERTIES PASS_REGULAR_EXPRESSION "${_vkc_skipped} Probe.GetsADevice")

  add_test(
    NAME vkc_testing_requires_the_layer
    COMMAND
      "${CMAKE_COMMAND}" -E env --unset=VKC_TEST_SYNC_VALIDATION
      --unset=VK_ADD_LAYER_PATH --unset=VK_INSTANCE_LAYERS
      --unset=VK_LOADER_LAYERS_ENABLE "VK_LAYER_PATH=${_vkc_no_layers}"
      VKC_TEST_VALIDATION=1 "${_vkc_probe}" --gtest_filter=Probe.GetsADevice)
  set_tests_properties(
    vkc_testing_requires_the_layer
    PROPERTIES PASS_REGULAR_EXPRESSION "validation is off"
               SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  add_test(
    NAME vkc_testing_shares_the_device
    COMMAND
      "${_vkc_probe}"
      --gtest_filter=Probe.MakesAFence:Probe.DestroysTheFenceOnTheSameDevice)
  set_tests_properties(vkc_testing_shares_the_device
                       PROPERTIES SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  add_test(NAME vkc_testing_reports_a_leak
           COMMAND "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0
                   "${_vkc_probe}" --gtest_filter=LeakProbe.LeavesABuffer)
  set_tests_properties(
    vkc_testing_reports_a_leak
    PROPERTIES PASS_REGULAR_EXPRESSION "an object a test never destroyed"
               SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  add_test(NAME vkc_testing_stops_a_fixture_chain
           COMMAND "${_vkc_probe}" --gtest_filter=LeafProbe.*)
  set_tests_properties(
    vkc_testing_stops_a_fixture_chain
    PROPERTIES PASS_REGULAR_EXPRESSION "the middle SetUp stops here"
               FAIL_REGULAR_EXPRESSION "carried on|the body ran"
               SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  add_test(NAME vkc_testing_own_instance_runs_sync_validation
           COMMAND "${CMAKE_COMMAND}" -E env VKC_TEST_SYNC_VALIDATION=1
                   "${_vkc_probe}" --gtest_filter=OwnInstanceProbe.*)
  set_tests_properties(vkc_testing_own_instance_runs_sync_validation
                       PROPERTIES SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  # A sibling without googletest, whose tests are main()s of their own, asks for
  # the test support and links core_test_policy alone: the package consumer,
  # configured that way, generates. Configure only, with the VMA this build
  # fetched.
  include(FetchContent)
  FetchContent_GetProperties(VulkanMemoryAllocator SOURCE_DIR _vkc_vma_source)
  add_test(
    NAME vkc_testing_policy_needs_no_googletest
    COMMAND
      "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/package_consumer" -B
      "${CMAKE_CURRENT_BINARY_DIR}/policy_consumer" -G "${CMAKE_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}" -DVKC_CONSUME=subdirectory
      "-DVKC_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -DVKC_WITH_VULKAN=ON
      -DVKC_CONSUMER_GTEST=OFF
      "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${_vkc_vma_source}")
  set_tests_properties(vkc_testing_policy_needs_no_googletest PROPERTIES TIMEOUT
                                                                         300)

  # Flipping VKC_WITH_VULKAN in a build directory reconfigures, either way:
  # whether the test support is built follows the tier and the tests, not a
  # default cached by the first configure. Configure only, in order, with the
  # dependencies this build fetched.
  FetchContent_GetProperties(googletest SOURCE_DIR _vkc_gtest_source)
  set(_vkc_step 0)
  foreach(_vkc_vulkan ON OFF ON)
    math(EXPR _vkc_next "${_vkc_step} + 1")
    set(_vkc_name vkc_testing_support_follows_vulkan_${_vkc_next})
    add_test(
      NAME ${_vkc_name}
      COMMAND
        "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}" -B
        "${CMAKE_CURRENT_BINARY_DIR}/vulkan_flip" -G "${CMAKE_GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -DVKC_WITH_VULKAN=${_vkc_vulkan}
        "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${_vkc_vma_source}"
        "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${_vkc_gtest_source}")
    set_tests_properties(${_vkc_name} PROPERTIES FIXTURES_SETUP
                                                 vkc_vulkan_flip_${_vkc_next})
    if(_vkc_step GREATER 0)
      set_tests_properties(${_vkc_name} PROPERTIES FIXTURES_REQUIRED
                                                   vkc_vulkan_flip_${_vkc_step})
    endif()
    set(_vkc_step ${_vkc_next})
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
