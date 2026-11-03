# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# --- core_net: the family's TCP sockets --------------------------------------
# IPv4 addresses and their checks, the TCP socket calls with every socket option
# and platform rule, and dial / Listener (DECISIONS.md, "Tiers" and the
# 2026-11-02 net entry). On core_base and the system's BSD sockets alone: no
# thread, no third-party library, no stream or media format. It needs nothing
# installed, so it is always built, as core_base is.
include(GenerateExportHeader)

set(_vkc_generated_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

add_library(core_net)
add_library(volumetric_kit::core_net ALIAS core_net)

target_sources(core_net PRIVATE address.cpp dial.cpp socket.cpp
                                socket_internal.hpp)

generate_export_header(
  core_net
  BASE_NAME
  VKC_NET
  EXPORT_MACRO_NAME
  VKC_NET_API
  EXPORT_FILE_NAME
  "${_vkc_generated_inc}/volumetric_kit/core/net/export.hpp")

# As core_base: a prefixed file name for a shared lib directory, and a
# MAJOR.MINOR soname before 1.0.
#
# TODO: from 1.0, SOVERSION ${PROJECT_VERSION_MAJOR} alone.
set_target_properties(
  core_net
  PROPERTIES OUTPUT_NAME volumetric_kit_core_net
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             POSITION_INDEPENDENT_CODE ON)

# With a static archive, VKC_NET_API must expand to nothing for consumers.
get_target_property(_vkc_type core_net TYPE)
if(_vkc_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(core_net PUBLIC VKC_NET_STATIC_DEFINE)
endif()

target_compile_features(core_net PUBLIC cxx_std_17)
# Catch allocation failures at the Status boundary even when a consuming
# application disables exceptions. Private: no public header reaches it.
target_compile_options(core_net PRIVATE -fexceptions)

target_include_directories(core_net PUBLIC "${PROJECT_SOURCE_DIR}/include"
                                           "${_vkc_generated_inc}")

# PUBLIC: the headers return Status / Result and UniqueFd.
target_link_libraries(core_net PUBLIC core_base)

vkc_target_warnings(core_net)
vkc_propagate_sanitizers(core_net)
