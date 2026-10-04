# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# GLSL -> SPIR-V at build time, and SPIR-V embedded in a target as a header: the
# family's one copy of what recon (vr_shaders / vr_embed), gfx (vg_shaders /
# vg_embed) and ios (through recon's) carried. Defined by the core's own
# CMakeLists when the vulkan tier is built, so a project that fetches the core
# can call them; installed beside the package config, which includes this file,
# so a find_package() consumer can too.
#
# vkc_compile_shaders(<target> [OUTPUT_DIR <dir>] [OUT_TARGET <var>] [TARGET_ENV
# <env>] [INCLUDE_DIRS <dir>...] [SPIRV_VAL_ARGS <arg>...] SHADERS <file>...)
#
# Compiles each shader to <dir>/<name>.spv (fill.comp -> fill.comp.spv), and
# makes <target> depend on the results, so building it recompiles a stale shader
# -- an edited #include too, through the compiler's depfile.
#
# * OUTPUT_DIR: default ${CMAKE_CURRENT_BINARY_DIR}/shaders.
# * OUT_TARGET: set to the custom target that produces the .spv, for a caller
#   that consumes them through a target of its own (vkc_embed_shaders).
# * TARGET_ENV: the SPIR-V environment, default vulkan1.2 (the tier's floor:
#   timeline semaphores, and scalar block layout for the compute ABI). gfx's
#   renderer passes vulkan1.3.
# * INCLUDE_DIRS: #include roots. recon passes its src/, so a shader spelt
#   "volumetric_kit/recon/core/shaders/color_common.glsl" reads like the C++
#   header path it sits beside.
# * SPIRV_VAL_ARGS: extra spirv-val flags, e.g. --scalar-block-layout for
#   shaders that read structs through scalar block layout.
#
# The compiler is glslc (shaderc), else glslangValidator: find_package(Vulkan)'s
# Vulkan_GLSLC_EXECUTABLE / Vulkan_GLSLANG_VALIDATOR_EXECUTABLE, else PATH. It
# is required only when this is called, so a build that compiles no shader needs
# none. spirv-val (SPIRV-Tools) validates each output when found, and its
# absence is reported once: skipping it silently looks exactly like passing.
function(vkc_compile_shaders target)
  cmake_parse_arguments(ARG "" "OUTPUT_DIR;OUT_TARGET;TARGET_ENV"
                        "INCLUDE_DIRS;SPIRV_VAL_ARGS;SHADERS" ${ARGN})
  if(NOT ARG_SHADERS)
    message(FATAL_ERROR "vkc_compile_shaders(${target}): no SHADERS given")
  endif()
  if(NOT ARG_OUTPUT_DIR)
    set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  endif()
  if(NOT ARG_TARGET_ENV)
    set(ARG_TARGET_ENV vulkan1.2)
  endif()

  if(Vulkan_GLSLC_EXECUTABLE)
    set(_compiler "${Vulkan_GLSLC_EXECUTABLE}")
    set(_mode glslc)
  elseif(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    set(_compiler "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")
    set(_mode glslang)
  else()
    find_program(VKC_GLSLC NAMES glslc)
    find_program(VKC_GLSLANG_VALIDATOR NAMES glslangValidator glslang)
    if(VKC_GLSLC)
      set(_compiler "${VKC_GLSLC}")
      set(_mode glslc)
    elseif(VKC_GLSLANG_VALIDATOR)
      set(_compiler "${VKC_GLSLANG_VALIDATOR}")
      set(_mode glslang)
    else()
      message(
        FATAL_ERROR
          "vkc_compile_shaders(${target}): no GLSL->SPIR-V compiler found. "
          "Install shaderc (glslc) or glslang -- both ship with the Vulkan SDK "
          "(macOS: brew install shaderc; Ubuntu: apt install glslc).")
    endif()
  endif()

  find_program(VKC_SPIRV_VAL NAMES spirv-val)
  if(NOT VKC_SPIRV_VAL AND NOT _vkc_spirv_val_reported)
    set(_vkc_spirv_val_reported
        TRUE
        CACHE INTERNAL "")
    message(
      STATUS
        "spirv-val not found: SPIR-V will be compiled but NOT validated "
        "(install SPIRV-Tools -- macOS: brew install spirv-tools; Ubuntu: apt "
        "install spirv-tools)")
  endif()

  set(_includes)
  foreach(_dir IN LISTS ARG_INCLUDE_DIRS)
    get_filename_component(_dir "${_dir}" ABSOLUTE)
    list(APPEND _includes "-I${_dir}")
  endforeach()

  set(_outputs)
  set(_names)
  foreach(_src IN LISTS ARG_SHADERS)
    get_filename_component(_name "${_src}" NAME)
    get_filename_component(_abs "${_src}" ABSOLUTE)
    # Outputs are keyed by file name, so two sources sharing one would clobber
    # each other.
    if(_name IN_LIST _names)
      message(
        FATAL_ERROR
          "vkc_compile_shaders(${target}): duplicate shader name '${_name}'")
    endif()
    list(APPEND _names "${_name}")

    set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")
    # Across calls too: two writing one path clobber each other under Make and
    # fail under Ninja.
    get_property(_all GLOBAL PROPERTY _vkc_shader_outputs)
    if(_out IN_LIST _all)
      message(
        FATAL_ERROR
          "vkc_compile_shaders(${target}): '${_out}' is produced by another "
          "vkc_compile_shaders() call; pass a distinct OUTPUT_DIR")
    endif()
    set_property(GLOBAL APPEND PROPERTY _vkc_shader_outputs "${_out}")

    set(_dep "${_out}.d")
    if(_mode STREQUAL glslc)
      set(_cmd
          "${_compiler}"
          "--target-env=${ARG_TARGET_ENV}"
          ${_includes}
          -MD
          -MF
          "${_dep}"
          -o
          "${_out}"
          "${_abs}")
    else()
      set(_cmd
          "${_compiler}"
          -V
          --target-env
          "${ARG_TARGET_ENV}"
          ${_includes}
          --depfile
          "${_dep}"
          -o
          "${_out}"
          "${_abs}")
    endif()
    set(_validate)
    if(VKC_SPIRV_VAL)
      set(_validate COMMAND "${VKC_SPIRV_VAL}" --target-env "${ARG_TARGET_ENV}"
                    ${ARG_SPIRV_VAL_ARGS} "${_out}")
    endif()

    # make_directory at build time, so a cleaned output directory is remade
    # without re-running CMake.
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
      COMMAND ${_cmd} ${_validate}
      DEPENDS "${_abs}"
      DEPFILE "${_dep}"
      COMMENT "Compiling shader ${_name}"
      VERBATIM)
    list(APPEND _outputs "${_out}")
  endforeach()

  # A uniquely named target per call, so calling more than once per target
  # works.
  get_property(_seq GLOBAL PROPERTY _vkc_shader_seq)
  if(NOT _seq)
    set(_seq 0)
  endif()
  math(EXPR _seq "${_seq} + 1")
  set_property(GLOBAL PROPERTY _vkc_shader_seq "${_seq}")
  add_custom_target(${target}_shaders_${_seq} DEPENDS ${_outputs})
  add_dependencies(${target} ${target}_shaders_${_seq})
  if(ARG_OUT_TARGET)
    set(${ARG_OUT_TARGET}
        "${target}_shaders_${_seq}"
        PARENT_SCOPE)
  endif()
endfunction()

# vkc_embed_shaders(<target> SYMBOL_PREFIX <prefix> [TARGET_ENV <env>]
# [INCLUDE_DIRS <dir>...] [SPIRV_VAL_ARGS <arg>...] SHADERS <file>...)
#
# Compiles each shader (as vkc_compile_shaders, whose options it passes on) and
# embeds the SPIR-V in <target> as a header on a PRIVATE, never-installed
# include path, so <target> ships its shaders with no runtime files. For
# <stem>.<stage> (integrate.comp) the header is <stem>_<stage>.spv.hpp,
# declaring at global scope `<prefix><stem>_<stage>_spv[]` -- 4-byte aligned, so
# it reinterprets as the `const uint32_t*` VkShaderModuleCreateInfo takes -- and
# `<prefix><stem>_<stage>_spv_size`, its byte count. <target>'s sources include
# the header by name.
#
# SYMBOL_PREFIX is required: the arrays are inline variables, which the linker
# merges by name, so two libraries in one binary embedding a fill.comp each
# under one name would share one of the two shaders.
function(vkc_embed_shaders target)
  cmake_parse_arguments(ARG "" "SYMBOL_PREFIX;TARGET_ENV"
                        "INCLUDE_DIRS;SPIRV_VAL_ARGS;SHADERS" ${ARGN})
  if(NOT ARG_SHADERS)
    message(FATAL_ERROR "vkc_embed_shaders(${target}): no SHADERS given")
  endif()
  if(NOT ARG_SYMBOL_PREFIX)
    message(
      FATAL_ERROR
        "vkc_embed_shaders(${target}): SYMBOL_PREFIX is required (e.g. vr_), "
        "so two libraries' shaders of one name cannot merge at link time")
  endif()

  set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/vkc_embed_spirv.cmake")
  set(_spv_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_embed_spv")
  set(_inc_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_embed_inc")
  set(_forward)
  if(ARG_TARGET_ENV)
    list(APPEND _forward TARGET_ENV "${ARG_TARGET_ENV}")
  endif()
  if(ARG_INCLUDE_DIRS)
    list(APPEND _forward INCLUDE_DIRS ${ARG_INCLUDE_DIRS})
  endif()
  if(ARG_SPIRV_VAL_ARGS)
    list(APPEND _forward SPIRV_VAL_ARGS ${ARG_SPIRV_VAL_ARGS})
  endif()
  vkc_compile_shaders(
    ${target}
    OUTPUT_DIR
    "${_spv_dir}"
    OUT_TARGET
    _compile_target
    ${_forward}
    SHADERS
    ${ARG_SHADERS})

  set(_headers)
  foreach(_src IN LISTS ARG_SHADERS)
    get_filename_component(_name "${_src}" NAME)
    string(REPLACE "." "_" _stem "${_name}")
    set(_spv "${_spv_dir}/${_name}.spv")
    set(_header "${_inc_dir}/${_stem}.spv.hpp")
    add_custom_command(
      OUTPUT "${_header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_inc_dir}"
      COMMAND
        ${CMAKE_COMMAND} "-DSPV=${_spv}"
        "-DSYMBOL=${ARG_SYMBOL_PREFIX}${_stem}_spv" "-DOUT=${_header}" -P
        "${_script}"
      DEPENDS "${_spv}" "${_script}"
      COMMENT "Embedding ${_name}.spv"
      VERBATIM)
    list(APPEND _headers "${_header}")
  endforeach()

  get_property(_seq GLOBAL PROPERTY _vkc_embed_seq)
  if(NOT _seq)
    set(_seq 0)
  endif()
  math(EXPR _seq "${_seq} + 1")
  set_property(GLOBAL PROPERTY _vkc_embed_seq "${_seq}")
  add_custom_target(${target}_embedded_shaders_${_seq} DEPENDS ${_headers})
  add_dependencies(${target} ${target}_embedded_shaders_${_seq})
  # The .spv are outputs of the compile target and inputs of this one, so both
  # can reach their rule; unordered, `make -j` runs it from both at once and
  # truncates the file (spirv-val: "Missing OpFunctionEnd"). Ninja dedups on its
  # global graph; this edge is for the Makefile generators.
  add_dependencies(${target}_embedded_shaders_${_seq} ${_compile_target})
  target_include_directories(${target} PRIVATE "${_inc_dir}")
endfunction()
