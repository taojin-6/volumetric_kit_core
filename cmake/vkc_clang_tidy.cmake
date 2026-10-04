# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# clang-tidy on every first-party target as it compiles, when VKC_CLANG_TIDY is
# ON: vkc_target_warnings() sets each target's CXX_CLANG_TIDY to the command
# built here. The checks live in .clang-tidy at the repository root, and any
# finding fails the build (WarningsAsErrors there).
#
# Example: cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVKC_CLANG_TIDY=ON
if(VKC_CLANG_TIDY)
  find_program(VKC_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(VKC_CLANG_TIDY_COMMAND "${VKC_CLANG_TIDY_EXE}")

  # CMake 4 no longer passes -isysroot on macOS, because Apple's clang finds the
  # SDK itself. An upstream clang-tidy (the pinned one from PyPI) does not, and
  # then cannot find the C++ standard headers, so hand it the SDK.
  if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
      COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE _vkc_sdk
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _vkc_sdk_result)
    if(_vkc_sdk_result EQUAL 0 AND _vkc_sdk)
      list(APPEND VKC_CLANG_TIDY_COMMAND "--extra-arg=-isysroot"
           "--extra-arg=${_vkc_sdk}")
    endif()
  endif()

  message(STATUS "clang-tidy enabled (VKC_CLANG_TIDY): ${VKC_CLANG_TIDY_EXE}")
endif()
