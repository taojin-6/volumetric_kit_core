# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Run by the vkc_require_core_call_* tests: cmake -DVKC_CALL=<call> -P <this>
# makes one call into cmake/vkc_require_core.cmake with no core added, which
# must fail before it looks for one when its arguments are wrong, and on the
# missing core otherwise.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/vkc_require_core.cmake")
cmake_language(EVAL CODE "${VKC_CALL}")
