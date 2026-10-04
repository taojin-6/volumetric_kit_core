// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/version.hpp"

namespace volumetric_kit::core {

const char* version_string() noexcept { return VKC_VERSION_STRING; }

int version_major() noexcept { return VKC_VERSION_MAJOR; }

int version_minor() noexcept { return VKC_VERSION_MINOR; }

int version_patch() noexcept { return VKC_VERSION_PATCH; }

}  // namespace volumetric_kit::core
