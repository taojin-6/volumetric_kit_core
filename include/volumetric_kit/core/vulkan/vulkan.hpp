// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file vulkan.hpp
/// @brief The one header through which first-party code includes Vulkan.
///
/// Every source in the core -- and in recon, gfx and ios once they build on
/// it -- reaches Vulkan through this file, never through `<vulkan/...>`
/// directly; the `vulkan-include-umbrella` pre-commit hook enforces it. The
/// loader choice (the link-time loader today, volk later for iOS and Android)
/// therefore stays a detail of this one line.

// TODO: switch iOS and Android to volk (VK_NO_PROTOTYPES, then volk.h here).
// UniqueHandle already takes a loaded entry point; Instance and Device must
// then call volkLoadInstance / volkLoadDevice.
#include <vulkan/vulkan.h>  // IWYU pragma: export

// The oldest Vulkan headers the tier supports (DECISIONS.md, "Vulkan headers
// come from the system"): 1.3.204, Ubuntu 22.04's. Apple needs 1.3.208, the
// first with VK_KHR_portability_enumeration, without which the loader hides
// MoltenVK's devices. VK_HEADER_VERSION alone decides it: it keeps counting
// across minor versions (1.2.203 came before 1.3.204), and
// VK_HEADER_VERSION_COMPLETE's casts are beyond the preprocessor. Keep these in
// step with VKC_VULKAN_MIN_VERSION, the build's configure-time check
// (CMakeLists.txt).
#if !defined(VK_HEADER_VERSION) || VK_HEADER_VERSION < 204
#error "volumetric_kit_core needs Vulkan headers 1.3.204 or newer"
#endif
#if defined(__APPLE__) && !defined(VK_KHR_portability_enumeration)
#error "volumetric_kit_core needs Vulkan headers 1.3.208 or newer on Apple"
#endif
