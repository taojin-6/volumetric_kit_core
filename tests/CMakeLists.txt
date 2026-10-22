# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# Group ordinary device tests within each process so the Vulkan fixture's
# instance/device registry is reused. Keep individual discovery by default for
# ctest -R, and leave the policy/environment probes isolated in either mode.
set(VKC_TEST_SHARDS
    0
    CACHE STRING "CTest shards per device-test binary (0: one per test)")
if(NOT VKC_TEST_SHARDS MATCHES "^(0|[1-9][0-9]*)$")
  message(FATAL_ERROR "VKC_TEST_SHARDS must be a non-negative integer")
endif()

function(_vkc_discover_device_tests target)
  if(VKC_TEST_SHARDS EQUAL 0)
    gtest_discover_tests(
      ${target}
      PROPERTIES
      TIMEOUT 120)
    return()
  endif()
  math(EXPR _last "${VKC_TEST_SHARDS} - 1")
  foreach(_index RANGE ${_last})
    set(_name "${target}.shard${_index}")
    add_test(NAME "${_name}" COMMAND ${target})
    # Do not match the GoogleTest skip banner: one capability skip must not hide
    # another case's failure, or a failure in the shared-device teardown. CI
    # requires a device; individual skips remain visible in gtest's output.
    set_tests_properties(
      "${_name}"
      PROPERTIES
        TIMEOUT 600 ENVIRONMENT
        "GTEST_TOTAL_SHARDS=${VKC_TEST_SHARDS};GTEST_SHARD_INDEX=${_index}")
  endforeach()
endfunction()

# Sets <out> to a regex matching <text> in a configure's output, however CMake
# wraps the message's lines.
function(_vkc_message_regex out text)
  string(REGEX REPLACE "([][.()*+?^$|])" "\\\\\\1" text "${text}")
  string(REPLACE " " "[ \n]+" text "${text}")
  set(${out}
      "${text}"
      PARENT_SCOPE)
endfunction()

# A test that runs CMake or the compiler rather than a test binary carries the
# label cmake, or ios for the iOS configures. What the cmake ones check is CMake
# logic no OS changes, and a fresh configure costs seconds on macOS, so CI runs
# them on one Linux leg, and the ios ones on macOS.

# base: Status / Result / VKC_TRY / VKC_ASSIGN, VKC_CHECK, the log sink,
# exception_status, file I/O, UniqueFd and the version API. Pure CPU, so these
# run on every machine. The timeout turns a deadlock in the log sink's locking
# into a failure instead of a hung job.
foreach(
  _vkc_test
  base_result
  base_check
  base_log
  base_version
  base_stage_metrics
  base_exception_status
  base_file_io
  base_unique_fd)
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
  set(_vkc_device_tests
      vulkan_device
      vulkan_allocator
      vulkan_allocation_budget
      vulkan_resources
      vulkan_compute
      vulkan_command_batch
      vulkan_timer
      vulkan_external_memory
      vulkan_shared_device)
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
    testing_vulkan_fixture
    testing_two_family_device
    testing_write_image)
    add_executable(vkc_${_vkc_test}_test ${_vkc_test}_test.cpp)
    target_link_libraries(
      vkc_${_vkc_test}_test PRIVATE volumetric_kit::core_test_support
                                    GTest::gtest_main)
    vkc_target_warnings(vkc_${_vkc_test}_test)
    if(_vkc_test IN_LIST _vkc_device_tests)
      _vkc_discover_device_tests(vkc_${_vkc_test}_test)
    else()
      gtest_discover_tests(
        vkc_${_vkc_test}_test
        PROPERTIES
        TIMEOUT 120)
    endif()
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

# The Vulkan-header floor (vulkan.hpp): compile vulkan_header_floor.cpp, which
# lowers the system headers' VK_HEADER_VERSION before vulkan.hpp checks it, one
# patch release below VKC_VULKAN_MIN_VERSION and at it. The first must fail with
# the floor's message and the second compile, so the header's floor and the
# build's agree. Compile only: nothing is linked or run.
if(VKC_WITH_VULKAN)
  string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+)$" "\\1" _vkc_floor
                       "${VKC_VULKAN_MIN_VERSION}")
  math(EXPR _vkc_below "${_vkc_floor} - 1")
  set(_vkc_sysroot)
  if(APPLE AND CMAKE_OSX_SYSROOT)
    set(_vkc_sysroot -isysroot "${CMAKE_OSX_SYSROOT}")
  endif()
  foreach(_vkc_version ${_vkc_below} ${_vkc_floor})
    add_test(
      NAME vkc_vulkan_header_floor_${_vkc_version}
      COMMAND
        "${CMAKE_CXX_COMPILER}" ${_vkc_sysroot} -std=c++17 -fsyntax-only
        "-I${PROJECT_SOURCE_DIR}/include" "-I${Vulkan_INCLUDE_DIR}"
        "-DVKC_TEST_HEADER_VERSION=${_vkc_version}"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_header_floor.cpp")
    set_tests_properties(vkc_vulkan_header_floor_${_vkc_version}
                         PROPERTIES LABELS cmake)
  endforeach()
  set_tests_properties(
    vkc_vulkan_header_floor_${_vkc_below}
    PROPERTIES PASS_REGULAR_EXPRESSION
               "needs Vulkan headers ${VKC_VULKAN_MIN_VERSION} or newer")
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
  # The fixtures' skip without a device (no_device in support/), told from their
  # other skips.
  set(_vkc_no_device "no Vulkan device for this test")

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

  # The two-family fixture: two tests in one process borrow one device, and
  # fixtures that differ in a requirement or level get their own; a buffer left
  # on it fails the run as it goes; requirements it cannot share or serve
  # headless fail SetUp; and it skips on one family. Without a device, each
  # skips.
  #
  # MoltenVK's specialized families (MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES) give
  # one family that does both graphics and compute, as lavapipe's single family
  # does, so the skip runs on both CI legs.
  add_test(
    NAME vkc_testing_shares_the_two_family_device
    COMMAND
      "${_vkc_probe}"
      --gtest_filter=TwoFamilyProbe.MakesTheDevice:TwoFamilyProbe.BorrowsTheSameDevice
  )
  set_tests_properties(vkc_testing_shares_the_two_family_device
                       PROPERTIES SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  # One device per key: either library's requirements, and the level, whose
  # probe runs apart as it needs the layer.
  add_test(
    NAME vkc_testing_keys_the_two_family_device
    COMMAND
      "${_vkc_probe}"
      "--gtest_filter=TwoFamilyProbe.MakesTheDevice:ComputeKeyProbe.*:GraphicsKeyProbe.*:SameKeyProbe.*"
  )
  add_test(
    NAME vkc_testing_keys_the_two_family_level
    COMMAND
      "${CMAKE_COMMAND}" -E env --unset=VKC_TEST_SYNC_VALIDATION
      "${_vkc_probe}"
      "--gtest_filter=TwoFamilyProbe.MakesTheDevice:SyncKeyProbe.*")
  set_tests_properties(
    vkc_testing_keys_the_two_family_device vkc_testing_keys_the_two_family_level
    PROPERTIES SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  add_test(NAME vkc_testing_reports_a_two_family_leak
           COMMAND "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0
                   "${_vkc_probe}" --gtest_filter=TwoFamilyProbe.LeavesABuffer)
  set_tests_properties(
    vkc_testing_reports_a_two_family_leak
    PROPERTIES PASS_REGULAR_EXPRESSION "an object a test never destroyed"
               SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  foreach(_vkc_misuse Chained Presenting)
    string(TOLOWER "${_vkc_misuse}" _vkc_suffix)
    add_test(NAME vkc_testing_two_family_refuses_${_vkc_suffix}
             COMMAND "${_vkc_probe}"
                     --gtest_filter=${_vkc_misuse}TwoFamilyProbe.FailsSetUp)
  endforeach()
  set_tests_properties(
    vkc_testing_two_family_refuses_chained
    PROPERTIES PASS_REGULAR_EXPRESSION "cannot compare a feature_chain"
               FAIL_REGULAR_EXPRESSION "the body ran" SKIP_REGULAR_EXPRESSION
               "${_vkc_skipped}")
  set_tests_properties(
    vkc_testing_two_family_refuses_presenting
    PROPERTIES PASS_REGULAR_EXPRESSION "is headless" FAIL_REGULAR_EXPRESSION
               "the body ran" SKIP_REGULAR_EXPRESSION "${_vkc_skipped}")

  add_test(
    NAME vkc_testing_skips_on_one_family
    COMMAND "${CMAKE_COMMAND}" -E env MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES=1
            "${_vkc_probe}" --gtest_filter=OneFamilyProbe.*)
  # Its pass is itself a skip, so only the skip without a device is one.
  set_tests_properties(
    vkc_testing_skips_on_one_family
    PROPERTIES PASS_REGULAR_EXPRESSION
               "puts both libraries on one queue family"
               FAIL_REGULAR_EXPRESSION
               "the body ran"
               SKIP_REGULAR_EXPRESSION
               "${_vkc_no_device}")

  # MoltenVK has several families of one queue each, so the cross-family tests
  # run there rather than skip, on its default families and on its specialized
  # ones, where the compute library's family does no graphics. Any skip but the
  # one without a device fails.
  if(APPLE)
    foreach(_vkc_specialized 0 1)
      set(_vkc_name
          vkc_testing_crosses_families_specialized_${_vkc_specialized})
      add_test(
        NAME ${_vkc_name}
        COMMAND
          "${CMAKE_COMMAND}" -E env
          MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES=${_vkc_specialized}
          "$<TARGET_FILE:vkc_testing_two_family_device_test>"
          --gtest_filter=CrossFamilyTest.*)
      set_tests_properties(
        ${_vkc_name} PROPERTIES FAIL_REGULAR_EXPRESSION "${_vkc_skipped}"
                                SKIP_REGULAR_EXPRESSION "${_vkc_no_device}")
    endforeach()
  endif()

  # A caller's legacy enables/disables must not override the session's sync
  # request. Check each spelling the layer accepts, through both the fixture and
  # the policy alone, with a real write-after-write hazard.
  foreach(_vkc_prefix VK_LAYER VK_KHRONOS_VALIDATION VK_VALIDATION VK)
    string(TOLOWER "${_vkc_prefix}" _vkc_suffix)
    foreach(_vkc_api fixture policy)
      set(_vkc_name vkc_testing_${_vkc_api}_overrides_${_vkc_suffix})
      if(_vkc_api STREQUAL "fixture")
        set(_vkc_command "$<TARGET_FILE:vkc_testing_vulkan_fixture_test>"
                         --gtest_filter=SyncValidatedTest.AHazardFailsTheTest)
      else()
        set(_vkc_command "$<TARGET_FILE:vkc_testing_vulkan_policy_test>")
      endif()
      add_test(
        NAME ${_vkc_name}
        COMMAND
          "${CMAKE_COMMAND}" -E env
          "${_vkc_prefix}_ENABLES=VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT"
          "${_vkc_prefix}_DISABLES=VK_VALIDATION_FEATURE_DISABLE_THREAD_SAFETY_EXT"
          ${_vkc_command})
      set_tests_properties(
        ${_vkc_name}
        PROPERTIES FAIL_REGULAR_EXPRESSION
                   "cannot be mixed"
                   SKIP_REGULAR_EXPRESSION
                   "${_vkc_skipped}"
                   SKIP_RETURN_CODE
                   77
                   TIMEOUT
                   120)
    endforeach()
  endforeach()

  # A sibling without googletest, whose tests are main()s of their own, asks for
  # the test support and links core_test_policy alone: the consumer, configured
  # that way, generates. Configure only, with the VMA this build fetched.
  include(FetchContent)
  FetchContent_GetProperties(VulkanMemoryAllocator SOURCE_DIR _vkc_vma_source)
  add_test(
    NAME vkc_testing_policy_needs_no_googletest
    COMMAND
      "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/consumer" -B
      "${CMAKE_CURRENT_BINARY_DIR}/vkc_testing_policy_needs_no_googletest" -G
      "${CMAKE_GENERATOR}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DVKC_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -DVKC_WITH_VULKAN=ON
      -DVKC_CONSUMER_GTEST=OFF
      "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${_vkc_vma_source}")
  set_tests_properties(vkc_testing_policy_needs_no_googletest
                       PROPERTIES TIMEOUT 300 LABELS cmake)

  # vkc_embed_shaders refuses a symbol another call already emits
  # (cmake/vkc_shaders.cmake): two libraries embedding fill.comp under
  # SYMBOL_PREFIX a_ both must fail the configure, and under a_ and b_ must not.
  # Configure only.
  _vkc_message_regex(
    _vkc_clash "'a_fill_comp_spv', which vkc_embed_shaders(vkc_first_library)")
  foreach(_vkc_prefix a_ b_)
    set(_vkc_name vkc_embed_shaders_prefixes_a_${_vkc_prefix})
    add_test(
      NAME ${_vkc_name}
      COMMAND
        "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/shader_symbols" -B
        "${CMAKE_CURRENT_BINARY_DIR}/${_vkc_name}" -G "${CMAKE_GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DVKC_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DVKC_SECOND_PREFIX=${_vkc_prefix}")
    set_tests_properties(${_vkc_name} PROPERTIES LABELS cmake)
    if(_vkc_prefix STREQUAL "a_")
      set_tests_properties(${_vkc_name} PROPERTIES PASS_REGULAR_EXPRESSION
                                                   "${_vkc_clash}")
    else()
      set_tests_properties(${_vkc_name} PROPERTIES FAIL_REGULAR_EXPRESSION
                                                   "${_vkc_clash}")
    endif()
  endforeach()

  # Flipping VKC_WITH_VULKAN in a build directory reconfigures, either way:
  # whether the test support is built follows the tier and the tests, not a
  # default cached by the first configure. Configure only, in order, with the
  # dependencies this build fetched and no config tier, so nothing is
  # downloaded.
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
        -DVKC_WITH_VULKAN=${_vkc_vulkan} -DVKC_WITH_CONFIG=OFF
        "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${_vkc_vma_source}"
        "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${_vkc_gtest_source}")
    set_tests_properties(
      ${_vkc_name} PROPERTIES FIXTURES_SETUP vkc_vulkan_flip_${_vkc_next}
                              LABELS cmake)
    if(_vkc_step GREATER 0)
      set_tests_properties(${_vkc_name} PROPERTIES FIXTURES_REQUIRED
                                                   vkc_vulkan_flip_${_vkc_step})
    endif()
    set(_vkc_step ${_vkc_next})
  endforeach()

  # cmake/vkc_ios.cmake as an iOS application meets it (ios_consumer/): Xcode
  # configures for iOS against the fixture's stubs of MoltenVK and Homebrew's
  # glm, with the VMA this build fetched, so only Xcode's iOS platform need be
  # installed (README.md, "Prerequisites"). Configure only. MoltenVK_ROOT comes
  # from a relative -D (consumer), from the environment (environment), from
  # neither in the environment case's build (rerun, as Xcode re-runs CMake), and
  # from -D beside an older one in the environment, re-pointing the consumer
  # case's build at a newer MoltenVK (reroot).
  if(APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    _vkc_message_regex(
      _vkc_frameworks
      "-framework Metal, -framework Foundation, -framework QuartzCore, -framework CoreGraphics, -framework IOSurface, -framework UIKit"
    )
    _vkc_message_regex(_vkc_holds "glm::glm's include directory holds [glm]")
    _vkc_message_regex(_vkc_upgraded
                       "after an upgrade, glm::glm has // glm 1.0.4")
    set(_vkc_glm "${_vkc_holds}.+${_vkc_upgraded}.+Generating done")
    foreach(
      _vkc_case
      consumer
      environment
      rerun
      reroot
      old_moltenvk
      no_version
      no_moltenvk
      no_glm)
      set(_vkc_name vkc_ios_${_vkc_case})
      set(_vkc_dir "${CMAKE_CURRENT_BINARY_DIR}/${_vkc_name}")
      set(_vkc_release 1.4.2)
      set(_vkc_env --unset=MoltenVK_ROOT)
      set(_vkc_args "")
      if(_vkc_case STREQUAL "rerun")
        set(_vkc_dir "${CMAKE_CURRENT_BINARY_DIR}/vkc_ios_environment")
      elseif(_vkc_case STREQUAL "reroot")
        set(_vkc_dir "${CMAKE_CURRENT_BINARY_DIR}/vkc_ios_consumer")
        set(_vkc_release 1.4.3)
        set(_vkc_env "MoltenVK_ROOT=${_vkc_dir}/moltenvk-1.4.1")
      elseif(_vkc_case STREQUAL "old_moltenvk")
        set(_vkc_release 1.4.1)
      elseif(_vkc_case STREQUAL "no_version")
        set(_vkc_release none)
      elseif(_vkc_case STREQUAL "no_glm")
        set(_vkc_args "-DVKC_GLM_INCLUDE_DIR=${_vkc_dir}/no_glm")
      endif()
      set(_vkc_root "${_vkc_dir}/moltenvk-${_vkc_release}")
      if(_vkc_case STREQUAL "consumer")
        # Against the directory the test runs in, this one.
        set(_vkc_args "-DMoltenVK_ROOT=${_vkc_name}/moltenvk-${_vkc_release}")
      elseif(_vkc_case STREQUAL "environment")
        set(_vkc_env "MoltenVK_ROOT=${_vkc_root}")
      elseif(NOT _vkc_case MATCHES "^(rerun|no_moltenvk)$")
        list(APPEND _vkc_args "-DMoltenVK_ROOT=${_vkc_root}")
      endif()
      add_test(
        NAME ${_vkc_name}
        COMMAND
          "${CMAKE_COMMAND}" -E env ${_vkc_env} "${CMAKE_COMMAND}" -S
          "${CMAKE_CURRENT_SOURCE_DIR}/ios_consumer" -B "${_vkc_dir}" -G Xcode
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
          -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
          "-DVKC_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
          "-DFETCHCONTENT_SOURCE_DIR_VULKANMEMORYALLOCATOR=${_vkc_vma_source}"
          ${_vkc_args})
      if(_vkc_case STREQUAL "old_moltenvk")
        _vkc_message_regex(
          _vkc_pass
          "MoltenVK 1.4.1 at ${_vkc_root} is older than 1.4.2, the oldest an iOS build takes."
        )
      elseif(_vkc_case STREQUAL "no_version")
        _vkc_message_regex(
          _vkc_pass
          "No MoltenVK version in ${_vkc_root}/include/MoltenVK/mvk_private_api.h."
        )
      elseif(_vkc_case STREQUAL "no_moltenvk")
        _vkc_message_regex(
          _vkc_pass
          "An iOS build links MoltenVK's iOS release statically, a prerequisite installed once. Point -DMoltenVK_ROOT, or the MoltenVK_ROOT environment variable, at the MoltenVK/ directory of MoltenVK-ios.tar 1.4.2 or newer (https://github.com/KhronosGroup/MoltenVK/releases). MoltenVK_ROOT is not set."
        )
      elseif(_vkc_case STREQUAL "no_glm")
        _vkc_message_regex(
          _vkc_pass
          "an iOS build takes glm's headers from the host, and none were found: install glm (brew install glm), or point -DVKC_GLM_INCLUDE_DIR at the directory holding glm/glm.hpp"
        )
        set(_vkc_pass "the core is added.+${_vkc_pass}")
      else()
        # The stub release 1.4.<n>'s headers say Vulkan 1.4.35<n>.
        string(REPLACE "1.4." "1.4.35" _vkc_headers "${_vkc_release}")
        _vkc_message_regex(
          _vkc_found "MoltenVK ${_vkc_release} (iOS, static): ${_vkc_root}")
        _vkc_message_regex(
          _vkc_vulkan
          "Vulkan ${_vkc_headers}, ${_vkc_root}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a"
        )
        set(_vkc_pass
            "${_vkc_found}.+${_vkc_vulkan}.+core_vulkan links [^\n]*${_vkc_frameworks}.+${_vkc_glm}"
        )
        set_tests_properties(${_vkc_name} PROPERTIES FAIL_REGULAR_EXPRESSION
                                                     "CMake Error")
      endif()
      set_tests_properties(
        ${_vkc_name} PROPERTIES PASS_REGULAR_EXPRESSION "${_vkc_pass}" TIMEOUT
                                300 LABELS ios)
    endforeach()
    foreach(_vkc_setup consumer environment)
      set_tests_properties(vkc_ios_${_vkc_setup}
                           PROPERTIES FIXTURES_SETUP vkc_ios_${_vkc_setup})
    endforeach()
    set_tests_properties(vkc_ios_reroot PROPERTIES FIXTURES_REQUIRED
                                                   vkc_ios_consumer)
    set_tests_properties(vkc_ios_rerun PROPERTIES FIXTURES_REQUIRED
                                                  vkc_ios_environment)
  endif()
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
  set_tests_properties(${_vkc_name} PROPERTIES LABELS cmake)
  if(_vkc_shared)
    set_tests_properties(${_vkc_name} PROPERTIES FAIL_REGULAR_EXPRESSION
                                                 "core_base is a static")
  else()
    set_tests_properties(${_vkc_name} PROPERTIES PASS_REGULAR_EXPRESSION
                                                 "core_base is a static")
  endif()
endforeach()

# vkc_require_core and vkc_core_version (cmake/vkc_require_core.cmake), as a
# sibling calls them: the consumer, on the core as a subproject (its base tier
# alone). Requiring this version must configure and read it back; requiring the
# next minor, or the vulkan or config tier from a core without it, must fail
# naming what was found and the fix. Configure only.
math(EXPR _vkc_next_minor "${PROJECT_VERSION_MINOR} + 1")
set(_vkc_too_new "${PROJECT_VERSION_MAJOR}.${_vkc_next_minor}.0")
_vkc_message_regex(
  _vkc_read
  "volumetric_kit_core ${PROJECT_VERSION} (MAJOR.MINOR ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})"
)
foreach(_vkc_require ${PROJECT_VERSION} ${_vkc_too_new} vulkan config)
  set(_vkc_name vkc_require_core_${_vkc_require})
  string(TOUPPER "${_vkc_require}" _vkc_tier)
  if(_vkc_require MATCHES "^(vulkan|config)$")
    set(_vkc_args -DVKC_CONSUMER_REQUIRE=${PROJECT_VERSION}
                  -DVKC_CONSUMER_REQUIRE_${_vkc_tier}=ON)
  else()
    set(_vkc_args -DVKC_CONSUMER_REQUIRE=${_vkc_require})
  endif()
  add_test(
    NAME ${_vkc_name}
    COMMAND
      "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/consumer" -B
      "${CMAKE_CURRENT_BINARY_DIR}/${_vkc_name}" -G "${CMAKE_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DVKC_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -DVKC_WITH_VULKAN=OFF
      ${_vkc_args})
  if(_vkc_require STREQUAL _vkc_too_new)
    _vkc_message_regex(
      _vkc_head
      "needs volumetric_kit_core ${_vkc_too_new} or newer, but the core at")
    _vkc_message_regex(_vkc_tail
                       "is ${PROJECT_VERSION}: pin v${_vkc_too_new} or newer")
    set(_vkc_pass "${_vkc_head}.+${_vkc_tail}")
  elseif(_vkc_require MATCHES "^(vulkan|config)$")
    _vkc_message_regex(
      _vkc_head
      "needs volumetric_kit_core's ${_vkc_require} tier, which the core at")
    _vkc_message_regex(_vkc_tail
                       "was built without: set VKC_WITH_${_vkc_tier} ON before")
    set(_vkc_pass "${_vkc_head}.+${_vkc_tail}")
  else()
    # The version read back, and a configure that finished.
    set(_vkc_pass "${_vkc_read}.+Generating done")
    set_tests_properties(${_vkc_name} PROPERTIES FAIL_REGULAR_EXPRESSION
                                                 "CMake Error")
  endif()
  set_tests_properties(${_vkc_name} PROPERTIES PASS_REGULAR_EXPRESSION
                                               "${_vkc_pass}" LABELS cmake)
endforeach()

# The same functions misused, or called before the core is added, must say so.
function(_vkc_call_test name call message)
  add_test(NAME ${name}
           COMMAND "${CMAKE_COMMAND}" "-DVKC_CALL=${call}" -P
                   "${CMAKE_CURRENT_SOURCE_DIR}/require_core_call.cmake")
  _vkc_message_regex(_regex "${message}")
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${_regex}"
                                          LABELS cmake)
endfunction()
_vkc_call_test(
  vkc_require_core_usage
  "vkc_require_core(0.1.0 VULKAN TOOLS)"
  "vkc_require_core(0.1.0 VULKAN TOOLS): use vkc_require_core(<min-version> [VULKAN] [CONFIG])"
)
_vkc_call_test(
  vkc_require_core_no_version "vkc_require_core()"
  "vkc_require_core(): use vkc_require_core(<min-version> [VULKAN] [CONFIG])")
_vkc_call_test(
  vkc_core_version_usage "vkc_core_version(_v MAJOR)"
  "vkc_core_version(_v MAJOR): use vkc_core_version(<out> [MAJOR_MINOR])")
_vkc_call_test(vkc_core_version_before_core "vkc_core_version(_v)"
               "vkc_core_version(_v): no volumetric_kit::core_base here")

# Beyond these configure tests, CI builds and runs consumer/ on the core as a
# subproject, as the sibling repos consume this one.

if(VKC_WITH_CONFIG)
  add_executable(vkc_config_test config_test.cpp)
  target_link_libraries(vkc_config_test PRIVATE volumetric_kit::core_config
                                                GTest::gtest_main)
  vkc_target_warnings(vkc_config_test)
  gtest_discover_tests(
    vkc_config_test
    PROPERTIES
    TIMEOUT 60)
endif()
