# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Orchestrator for the component tiers. Each subdirectory owns its own target;
# the dependency rule (a tier may depend only on the tiers it names in
# DECISIONS.md) is expressed by what each links.
add_subdirectory(volumetric_kit/core/base)
if(VKC_WITH_CONFIG)
  add_subdirectory(volumetric_kit/core/config)
endif()
if(VKC_WITH_VULKAN)
  add_subdirectory(volumetric_kit/core/vulkan)
endif()

# --- volumetric_kit::core: umbrella ------------------------------------------
# Consumers link `volumetric_kit::core` and get whatever tiers are built. The
# target is named for the package, not `core`, because a FetchContent build
# shares one target namespace with its consumer.
add_library(volumetric_kit_core INTERFACE)
add_library(volumetric_kit::core ALIAS volumetric_kit_core)
target_link_libraries(volumetric_kit_core INTERFACE core_base)
if(VKC_WITH_CONFIG)
  target_link_libraries(volumetric_kit_core INTERFACE core_config)
endif()
if(VKC_WITH_VULKAN)
  target_link_libraries(volumetric_kit_core INTERFACE core_vulkan)
endif()
