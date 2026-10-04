# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Sanitizer flags, applied GLOBALLY (compile + link) when VKC_SANITIZE is set.
#
# Unlike vkc_warnings (per-target PRIVATE), sanitizers are applied globally on
# purpose: the flags must reach the link line, and ASan's ABI is contagious --
# mixing a sanitized library with an un-sanitized executable is broken. For a
# consumer's targets, see vkc_propagate_sanitizers() below. Including this
# before add_subdirectory(third_party) also instruments the vendored static
# deps, which otherwise raise alloc/dealloc-mismatch false positives when linked
# into a sanitized test binary. Off by default, so normal builds are unaffected.
#
# Example: cmake -B build -DCMAKE_BUILD_TYPE=Debug
# -DVKC_SANITIZE="address;undefined"
if(VKC_SANITIZE)
  # address;undefined -> address,undefined (the -fsanitize= argument form).
  list(JOIN VKC_SANITIZE "," _vkc_sanitize_list)
  set(_vkc_sanitize_flags
      -fsanitize=${_vkc_sanitize_list}
      -fno-omit-frame-pointer # readable sanitizer stack traces
      -fno-sanitize-recover=all) # a finding fails the run, matching -Werror's
                                 # posture

  add_compile_options(${_vkc_sanitize_flags})
  add_link_options(${_vkc_sanitize_flags})

  message(STATUS "Sanitizers enabled (VKC_SANITIZE): ${VKC_SANITIZE}")
endif()

# add_compile_options/add_link_options reach only this project's directories.
# When the core is a subproject (add_subdirectory, FetchContent), the consumer's
# own targets would compile and link without the flags -- and a test executable
# linking a sanitized static core_base then fails with undefined __asan_*
# symbols. So the bottom tier, which everything links, also carries the flags in
# its build-tree usage requirements; the installed package never does. A
# top-level build has them globally already.
function(vkc_propagate_sanitizers target)
  if(VKC_SANITIZE AND NOT PROJECT_IS_TOP_LEVEL)
    target_compile_options(
      ${target} INTERFACE "$<BUILD_INTERFACE:${_vkc_sanitize_flags}>")
    target_link_options(${target} INTERFACE
                        "$<BUILD_INTERFACE:${_vkc_sanitize_flags}>")
  endif()
endfunction()
