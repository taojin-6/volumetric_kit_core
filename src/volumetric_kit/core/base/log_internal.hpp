// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Private to core_base (not installed, not exported): the pieces of log.cpp
// that check.cpp needs to report a failed check without trusting the
// installed handler.

#include <string_view>

#include "volumetric_kit/core/base/log.hpp"

namespace volumetric_kit::core::detail {

// The source the core's own diagnostics (failed checks) carry.
inline constexpr std::string_view kCoreLogSource = "core";

// The built-in sink: warnings and errors to stderr, quieter levels dropped.
void default_sink(LogLevel level, std::string_view source,
                  std::string_view message);

// True while this thread is inside a call to an installed handler.
bool in_log_handler() noexcept;

}  // namespace volumetric_kit::core::detail
