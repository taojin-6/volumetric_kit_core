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
/// alike, and each message names the library that emitted it (its `source`),
/// so a handler can still tell them apart.
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

/// @brief A diagnostic sink: receives each message with its severity and the
///        library that emitted it.
///
/// One installed handler object serves every call, so state it keeps (a
/// counter, a buffer) persists from call to call. Calls arrive concurrently
/// from every thread that logs, so that state must be synchronized.
///
/// @code
/// set_log_handler([](LogLevel level, std::string_view source,
///                    std::string_view message) {
///   if (level >= LogLevel::Warning) os_log_message(source, message);
/// });
/// @endcode
using LogHandler = std::function<void(LogLevel level, std::string_view source,
                                      std::string_view message)>;

/// @brief Install the diagnostic sink. Thread-safe.
///
/// Returns only once no other thread is still running the previous handler,
/// so whatever that handler references may be destroyed as soon as this
/// returns. Calls to it on *this* thread -- when this is called from inside a
/// handler -- finish afterwards, on the handler they started with. The
/// previous handler is destroyed without the internal lock held, so its
/// destructor may log. A handler must not wait for a thread that may be
/// calling `set_log_handler`, which would be waiting for that handler.
///
/// @param handler  The sink; an empty (default-constructed) handler restores
///                 the built-in default (warnings and errors to stderr).
VKC_BASE_API void set_log_handler(LogHandler handler);

/// @brief Emit a diagnostic through the current handler, or the default sink.
///        Thread-safe, and safe to call from inside a handler.
/// @param level    The message's severity.
/// @param source   The emitting library or component, e.g. `"recon"`; the
///                 default sink prints it as `[<source> <level>]`.
/// @param message  The message; it need not be NUL-terminated.
VKC_BASE_API void log_message(LogLevel level, std::string_view source,
                              std::string_view message);

}  // namespace volumetric_kit::core
