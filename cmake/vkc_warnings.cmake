# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Shared compiler-warning flags, applied PRIVATE so they never leak to consumers
# (a consumer's build should not inherit our warning policy), and clang-tidy
# when VKC_CLANG_TIDY is ON. Used as `vkc_target_warnings(<target>)` on every
# first-party target.
function(vkc_target_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(VKC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(VKC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(VKC_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY
                                               "${VKC_CLANG_TIDY_COMMAND}")
  endif()
endfunction()
