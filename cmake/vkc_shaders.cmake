# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# GLSL -> SPIR-V at build time, and SPIR-V embedded in a target as a header: the
# family's one copy of what recon (vr_shaders / vr_embed), gfx (vg_shaders /
# vg_embed) and ios (through recon's) carried. Defined by the core's own
# CMakeLists when the vulkan tier is built, so a project that fetches the core
# can call them; installed beside the package config, which includes this file,
# so a find_package() consumer can too.
#
# vkc_compile_shaders(<target> [OUTPUT_DIR <dir>] [TARGET_ENV <env>]
# [INCLUDE_DIRS <dir>...] [SPIRV_VAL_ARGS <arg>...] SHADERS <file>...)
#
# Compiles each shader to <dir>/<name>.spv (fill.comp -> fill.comp.spv), and
# makes <target> depend on the results, so building it recompiles a stale shader
# -- an edited #include too, through the compiler's depfile.
#
# * OUTPUT_DIR: default ${CMAKE_CURRENT_BINARY_DIR}/shaders.
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
# absence is reported once: skipping it silently looks exactly like passing. A
# shader that fails validation fails every build until fixed: the generators
# delete (Make) or rerun (Ninja, Xcode) a command that failed.
function(vkc_compile_shaders target)
  cmake_parse_arguments(ARG "" "OUTPUT_DIR;TARGET_ENV"
                        "INCLUDE_DIRS;SPIRV_VAL_ARGS;SHADERS" ${ARGN})
  if(NOT ARG_SHADERS)
    message(FATAL_ERROR "vkc_compile_shaders(${target}): no SHADERS given")
  endif()
  if(NOT ARG_OUTPUT_DIR)
    set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  endif()
  _vkc_add_shader_commands(
    _outputs
    "vkc_compile_shaders(${target})"
    "${ARG_OUTPUT_DIR}"
    "${ARG_TARGET_ENV}"
    INCLUDE_DIRS
    ${ARG_INCLUDE_DIRS}
    SPIRV_VAL_ARGS
    ${ARG_SPIRV_VAL_ARGS}
    SHADERS
    ${ARG_SHADERS})

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
endfunction()

# vkc_embed_shaders(<target> SYMBOL_PREFIX <prefix> [TARGET_ENV <env>]
# [INCLUDE_DIRS <dir>...] [SPIRV_VAL_ARGS <arg>...] SHADERS <file>...)
#
# Compiles each shader (as vkc_compile_shaders, whose options it takes) and
# embeds the SPIR-V in <target> as a header on a PRIVATE, never-installed
# include path, so <target> ships its shaders with no runtime files. Each header
# is named for its shader's file name made a C identifier -- every character but
# a letter, digit or underscore becomes one: integrate.comp gives
# integrate_comp.spv.hpp, declaring at global scope
# `<prefix>integrate_comp_spv[]` -- 4-byte aligned, so it reinterprets as the
# `const uint32_t*` VkShaderModuleCreateInfo takes -- and
# `<prefix>integrate_comp_spv_size`, its byte count. <target>'s sources include
# the header by name. Two shaders of one target whose names make one identifier
# (a.b.comp and a_b.comp) are refused.
#
# <target> may be an INTERFACE library: it then carries the include path, and
# the dependency on the headers, to every target of the build that links it, so
# several targets share one compile of the same shaders.
#
# SYMBOL_PREFIX is required: the arrays are inline variables, which the linker
# merges by name, so two libraries in one binary embedding a fill.comp each
# under one name would share one of the two shaders. A symbol another call in
# the build already emits is refused, so each library takes a prefix of its own.
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
  # The .spv rules and the header rules all belong to the one target below: a
  # rule two targets can reach races under Make and is refused by Xcode.
  _vkc_add_shader_commands(
    _spvs
    "vkc_embed_shaders(${target})"
    "${_spv_dir}"
    "${ARG_TARGET_ENV}"
    INCLUDE_DIRS
    ${ARG_INCLUDE_DIRS}
    SPIRV_VAL_ARGS
    ${ARG_SPIRV_VAL_ARGS}
    SHADERS
    ${ARG_SHADERS})

  set(_headers)
  foreach(_src IN LISTS ARG_SHADERS)
    get_filename_component(_name "${_src}" NAME)
    string(MAKE_C_IDENTIFIER "${_name}" _stem)
    set(_header "${_inc_dir}/${_stem}.spv.hpp")
    # Across calls on one target too, which share the include directory.
    get_property(_all GLOBAL PROPERTY _vkc_embed_headers)
    if(_header IN_LIST _all)
      message(
        FATAL_ERROR
          "vkc_embed_shaders(${target}): '${_name}' embeds as '${_stem}', as "
          "another shader of this target does; rename one")
    endif()
    set_property(GLOBAL APPEND PROPERTY _vkc_embed_headers "${_header}")
    # Across targets too: the header check above cannot see two targets' shaders
    # of one name, whose headers lie in two include directories.
    set(_symbol "${ARG_SYMBOL_PREFIX}${_stem}_spv")
    get_property(_owner GLOBAL PROPERTY _vkc_embed_symbol_${_symbol})
    if(_owner)
      message(
        FATAL_ERROR
          "vkc_embed_shaders(${target}): '${_name}' embeds as '${_symbol}', "
          "which vkc_embed_shaders(${_owner}) already emits, and the linker "
          "would merge the two: give each library its own SYMBOL_PREFIX")
    endif()
    set_property(GLOBAL PROPERTY _vkc_embed_symbol_${_symbol} "${target}")
    add_custom_command(
      OUTPUT "${_header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_inc_dir}"
      COMMAND ${CMAKE_COMMAND} "-DSPV=${_spv_dir}/${_name}.spv"
              "-DSYMBOL=${_symbol}" "-DOUT=${_header}" -P "${_script}"
      DEPENDS "${_spv_dir}/${_name}.spv" "${_script}"
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
  add_custom_target(${target}_embedded_shaders_${_seq} DEPENDS ${_spvs}
                                                               ${_headers})
  add_dependencies(${target} ${target}_embedded_shaders_${_seq})
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    target_include_directories(${target}
                               INTERFACE "$<BUILD_INTERFACE:${_inc_dir}>")
  else()
    target_include_directories(${target} PRIVATE "${_inc_dir}")
  endif()
endfunction()

# _vkc_add_shader_commands(<out_var> <caller> <output_dir> <target_env>
# [INCLUDE_DIRS <dir>...] [SPIRV_VAL_ARGS <arg>...] SHADERS <file>...)
#
# Internal to the two above: adds the command that compiles, and validates, each
# shader to <output_dir>/<name>.spv, and sets <out_var> to the outputs, for the
# caller to hang on one custom target of its own. <caller> names it in errors;
# an empty <target_env> takes the default.
function(_vkc_add_shader_commands out_var caller output_dir target_env)
  cmake_parse_arguments(ARG "" "" "INCLUDE_DIRS;SPIRV_VAL_ARGS;SHADERS" ${ARGN})
  if(NOT target_env)
    set(target_env vulkan1.2)
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
          "${caller}: no GLSL->SPIR-V compiler found. "
          "Install shaderc (glslc) or glslang -- both ship with the Vulkan SDK "
          "(macOS: brew install shaderc; Ubuntu: apt install glslc, or "
          "glslang-tools on 22.04).")
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
      message(FATAL_ERROR "${caller}: duplicate shader name '${_name}'")
    endif()
    list(APPEND _names "${_name}")

    set(_out "${output_dir}/${_name}.spv")
    # Across calls too: two writing one path clobber each other under Make and
    # fail under Ninja.
    get_property(_all GLOBAL PROPERTY _vkc_shader_outputs)
    if(_out IN_LIST _all)
      message(
        FATAL_ERROR
          "${caller}: '${_out}' is produced by another vkc_compile_shaders() "
          "or vkc_embed_shaders() call; pass a distinct OUTPUT_DIR")
    endif()
    set_property(GLOBAL APPEND PROPERTY _vkc_shader_outputs "${_out}")

    set(_dep "${_out}.d")
    if(_mode STREQUAL glslc)
      set(_cmd
          "${_compiler}"
          "--target-env=${target_env}"
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
          "${target_env}"
          ${_includes}
          --depfile
          "${_dep}"
          -o
          "${_out}"
          "${_abs}")
    endif()
    set(_validate)
    if(VKC_SPIRV_VAL)
      set(_validate COMMAND "${VKC_SPIRV_VAL}" --target-env "${target_env}"
                    ${ARG_SPIRV_VAL_ARGS} "${_out}")
    endif()

    # make_directory at build time, so a cleaned output directory is remade
    # without re-running CMake.
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
      COMMAND ${_cmd} ${_validate}
      DEPENDS "${_abs}"
      DEPFILE "${_dep}"
      COMMENT "Compiling shader ${_name}"
      VERBATIM)
    list(APPEND _outputs "${_out}")
  endforeach()
  set(${out_var}
      "${_outputs}"
      PARENT_SCOPE)
endfunction()
