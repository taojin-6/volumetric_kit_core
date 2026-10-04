// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file log.hpp
/// @brief A pluggable logging seam: one caller-installable diagnostic handler
///        for the whole family.
///
/// The libraries impose no logging framework on consumers: they emit through a
/// handler the application can install, defaulting to stderr for warnings and
/// errors. Because every sibling library links this one tier, a single
/// @ref set_log_handler call routes calib's, recon's and gfx's diagnostics
/// alike.
///
/// The handler is process-global state, so `core_base` must be linked into a
/// process exactly once (see DECISIONS.md, "One instance per process").

#include <functional>
#include <string_view>

#include "volumetric_kit/core/base/export.hpp"

namespace volumetric_kit::core {

/// @brief Severity of a diagnostic passed to a @ref LogHandler.
///
/// The built-in default sink emits @ref LogLevel::Warning and
/// @ref LogLevel::Error to stderr and drops @ref LogLevel::Debug and
/// @ref LogLevel::Info; an installed handler receives every level and decides
/// for itself.
enum class LogLevel {
  Debug,    ///< Verbose developer tracing; dropped by the default sink.
  Info,     ///< Normal progress information; dropped by the default sink.
  Warning,  ///< A recoverable problem; emitted to stderr by the default sink.
  Error,    ///< A failure; emitted to stderr by the default sink.
};

/// @brief A diagnostic sink: receives each message with its severity.
///
/// @code
/// set_log_handler([](LogLevel level, std::string_view message) {
///   if (level >= LogLevel::Warning) os_log_message(message);
/// });
/// @endcode
using LogHandler = std::function<void(LogLevel, std::string_view)>;

/// @brief Install the diagnostic sink. Thread-safe.
/// @param handler  The sink; an empty (default-constructed) handler restores
///                 the built-in default (warnings and errors to stderr).
VKC_BASE_API void set_log_handler(LogHandler handler);

/// @brief Emit a diagnostic through the current handler, or the default sink.
///        Thread-safe, and safe to call from inside a handler.
/// @param level    The message's severity.
/// @param message  The message; it need not be NUL-terminated.
VKC_BASE_API void log_message(LogLevel level, std::string_view message);

}  // namespace volumetric_kit::core
