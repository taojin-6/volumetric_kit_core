# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Writes a C++ header embedding a compiled SPIR-V binary, for vkc_embed_shaders
# (vkc_shaders.cmake), as a script step:
#
# cmake -DSPV=<in.spv> -DSYMBOL=<name> -DOUT=<out.hpp> -P vkc_embed_spirv.cmake
#
# The header declares, at global scope, `alignas(4) inline constexpr unsigned
# char <SYMBOL>[]` -- aligned so it reinterprets as the `const uint32_t*`
# VkShaderModuleCreateInfo takes -- and `inline constexpr std::size_t
# <SYMBOL>_size`, the byte count.
if(NOT SPV
   OR NOT SYMBOL
   OR NOT OUT)
  message(FATAL_ERROR "vkc_embed_spirv.cmake: SPV, SYMBOL and OUT are required")
endif()

file(READ "${SPV}" _hex HEX)
string(LENGTH "${_hex}" _hex_length)
math(EXPR _bytes "${_hex_length} / 2")
# An empty input would emit a zero-size array, ill-formed under -Wpedantic. Fail
# here, naming the file, instead of cryptically at compile.
if(_bytes EQUAL 0)
  message(
    FATAL_ERROR "vkc_embed_spirv.cmake: '${SPV}' is empty -- nothing to embed")
endif()
# "07230203..." -> "0x07,0x23,0x02,0x03,..." (a trailing comma is a valid
# initializer).
string(REGEX REPLACE "(..)" "0x\\1," _array "${_hex}")

file(
  WRITE "${OUT}"
  "// Generated from ${SPV} by vkc_embed_spirv.cmake -- do not edit.\n"
  "#pragma once\n"
  "#include <cstddef>\n"
  "alignas(4) inline constexpr unsigned char ${SYMBOL}[] = {${_array}};\n"
  "inline constexpr std::size_t ${SYMBOL}_size = ${_bytes};\n")
