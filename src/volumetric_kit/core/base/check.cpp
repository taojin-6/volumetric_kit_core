// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/check.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

#include "log_internal.hpp"
#include "volumetric_kit/core/base/log.hpp"

namespace volumetric_kit::core::detail {
namespace {

// The failure this thread is reporting through the handler, if any.
thread_local const std::string* t_reporting = nullptr;

}  // namespace

void check_failed(const char* file, int line, const char* expr,
                  std::string_view msg) {
  std::string text = "contract check failed: ";
  text.append(msg.data(), msg.size());
  text += " [";
  text += expr;
  text += "] at ";
  text += file;
  text += ':';
  text += std::to_string(line);

  // A check that fails inside the log handler, or while the handler reports an
  // earlier failure, must not go back through that handler: it would recurse
  // until the stack overflowed, and the crash would never show this message.
  // Write straight to stderr instead, the earlier failure first.
  if (t_reporting != nullptr || in_log_handler()) {
    if (t_reporting != nullptr) {
      default_sink(LogLevel::Error, kCoreLogSource, *t_reporting);
    }
    default_sink(LogLevel::Error, kCoreLogSource, text);
    std::abort();
  }
  t_reporting = &text;
  log_message(LogLevel::Error, kCoreLogSource, text);
  std::abort();
}

}  // namespace volumetric_kit::core::detail
