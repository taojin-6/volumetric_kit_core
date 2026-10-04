// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/log.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace volumetric_kit::core {
namespace {

// The installed handler and the mutex guarding it. Heap-allocated and never
// destroyed, so the sink outlives every consumer: a static object may log from
// its destructor during process teardown, and a function-local static here
// could lock an already-destroyed mutex (static destruction-order UB).
struct LogState {
  std::mutex mutex;
  LogHandler handler;  // empty => fall back to the default stderr sink
};

LogState& state() {
  static LogState* s = new LogState();
  return *s;
}

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warning:
      return "warning";
    case LogLevel::Error:
      return "error";
  }
  return "?";
}

// Default sink: warnings and errors to stderr, quieter levels dropped.
void default_sink(LogLevel level, std::string_view message) {
  if (level == LogLevel::Warning || level == LogLevel::Error) {
    // Compose the whole line and write it with one fwrite, so concurrent calls
    // do not interleave. The body is appended by its exact length rather than
    // printf's %.*s: a string_view need not be NUL-terminated, and its size can
    // exceed INT_MAX.
    std::string line = "[volumetric_kit ";
    line += level_name(level);
    line += "] ";
    line.append(message.data(), message.size());
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
}

}  // namespace

void set_log_handler(LogHandler handler) {
  LogState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.handler = std::move(handler);
}

void log_message(LogLevel level, std::string_view message) {
  // Copy the handler out under the lock, then call it unlocked: a handler may
  // log re-entrantly without deadlocking, and a concurrent set_log_handler
  // cannot destroy the callable mid-call, since this copy keeps it alive.
  LogHandler handler;
  {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    handler = s.handler;
  }
  if (handler) {
    handler(level, message);
  } else {
    default_sink(level, message);
  }
}

}  // namespace volumetric_kit::core
