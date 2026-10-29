# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Run by vkc_export_glsl_rebuild: cmake -DVKC_SOURCE_DIR=<core>
# -DVKC_WORK_DIR=<dir> -DVKC_GENERATOR=<generator> -DVKC_CXX_COMPILER=<compiler>
# -P <this> copies glsl_export/ into <dir> and builds it. A reconfigure must
# then compile no shader, as vkc_export_glsl keeps the times of the copies it
# keeps; and an edit to the exported scale.glsl must reach the kernel that
# includes it, as its copy is remade and the kernel recompiled. Each step waits
# a second first: the make macOS ships compares times to the second.
cmake_minimum_required(VERSION 3.21)

set(_project "${VKC_WORK_DIR}/project")
set(_build "${VKC_WORK_DIR}/build")
set(_spv "${_build}/shaders/scaled.comp.spv")
# A fresh copy each run: the last one edited scale.glsl.
file(REMOVE_RECURSE "${VKC_WORK_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/glsl_export/" DESTINATION "${_project}")

function(_vkc_configure)
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${_project}" -B "${_build}" -G "${VKC_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${VKC_CXX_COMPILER}"
      "-DVKC_SOURCE_DIR=${VKC_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()

# Builds the copy and sets <out> to what the build printed.
function(_vkc_build out)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "the build in '${_build}' failed:\n${_output}")
  endif()
  set(${out}
      "${_output}"
      PARENT_SCOPE)
endfunction()

function(_vkc_wait)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1 COMMAND_ERROR_IS_FATAL
                          ANY)
endfunction()

_vkc_configure()
_vkc_build(_output)
file(SHA256 "${_spv}" _before)

_vkc_wait()
_vkc_configure()
_vkc_build(_output)
if(_output MATCHES "Compiling shader")
  message(FATAL_ERROR "a reconfigure with no change recompiled a shader, so "
                      "the export remade a copy it kept:\n${_output}")
endif()

_vkc_wait()
set(_scale "${_project}/shaders/scale.glsl")
file(READ "${_scale}" _source)
string(REPLACE "2.0 * x" "3.0 * x" _edited "${_source}")
if(_edited STREQUAL _source)
  message(FATAL_ERROR "'${_scale}' has no '2.0 * x' to edit")
endif()
file(WRITE "${_scale}" "${_edited}")
_vkc_build(_output)
file(SHA256 "${_spv}" _after)
if(_after STREQUAL _before)
  message(FATAL_ERROR "an edit to the exported scale.glsl left '${_spv}' as "
                      "it was, so the export kept a stale copy:\n${_output}")
endif()
message(STATUS "vkc_export_glsl_rebuild: passed")
