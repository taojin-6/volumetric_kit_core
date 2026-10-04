# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Orchestrator for the component tiers. Each subdirectory owns its own target
# and installs itself into the shared volumetric_kit_coreTargets export set; the
# dependency rule (a tier may depend only on the tiers it names in DECISIONS.md)
# is expressed by what each links.
include(GNUInstallDirs)

add_subdirectory(volumetric_kit/core/base)
# TODO: add_subdirectory() for vulkan / camera / sensor as those tiers land (see
# DECISIONS.md "Tiers").

# --- volumetric_kit::core: umbrella ------------------------------------------
# Consumers link `volumetric_kit::core` and get whatever tiers are built. The
# target is named for the package, not `core`, because a FetchContent build
# shares one target namespace with its consumer; EXPORT_NAME keeps the installed
# name short.
add_library(volumetric_kit_core INTERFACE)
add_library(volumetric_kit::core ALIAS volumetric_kit_core)
set_target_properties(volumetric_kit_core PROPERTIES EXPORT_NAME core)
target_link_libraries(volumetric_kit_core INTERFACE core_base)

# --- Install / export --------------------------------------------------------
if(VKC_INSTALL)
  install(TARGETS volumetric_kit_core EXPORT volumetric_kit_coreTargets)

  install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING
    PATTERN "*.hpp")

  install(
    EXPORT volumetric_kit_coreTargets
    NAMESPACE volumetric_kit::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/volumetric_kit_core")

  include(CMakePackageConfigHelpers)
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/volumetric_kit_coreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/volumetric_kit_coreConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/volumetric_kit_core")
  # Before 1.0 a minor release may break the API, so only the same minor version
  # satisfies a find_package() request.
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/volumetric_kit_coreConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/volumetric_kit_coreConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/volumetric_kit_coreConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/volumetric_kit_core")
endif()
