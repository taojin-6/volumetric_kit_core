# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# An installed core older than 0.6.1, as the sibling's package config reads it
# after the consumer added this build's core from source. Its targets file finds
# the targets defined and returns. Its vkc_require_core.cmake has no guard and
# defines the command pair again, in the shape 0.1.0 to 0.6.0 had; the second
# definition replaces this function at once, so it never runs.
function(_vkc_require_core)
  message(FATAL_ERROR "the pre-0.6.1 _vkc_require_core ran")
endfunction()

macro(vkc_require_core)
  _vkc_require_core(${ARGV})
  if(_vkc_require_core_refused)
    unset(_vkc_require_core_refused)
    return()
  endif()
endmacro()
