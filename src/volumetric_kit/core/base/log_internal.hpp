// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Private to core_base (not installed, not exported): the pieces of log.cpp
// that check.cpp needs to report a failed check without trusting the
// installed handler, and the default sink's split for the unified log, inline
// so the host tests reach it (tests/base_log_test.cpp).

#include <cstddef>
#include <string_view>

#include "volumetric_kit/core/base/log.hpp"

namespace volumetric_kit::core::detail {

// The source the core's own diagnostics (failed checks) carry.
inline constexpr std::string_view kCoreLogSource = "core";

// The most text the default sink puts in one unified-log entry. os/log.h
// states "a physical cap of 1024 bytes per log line for dynamic content",
// past which the text is truncated before it is written to disk; that cap
// counts the entry's encoding too, and with the sink's "%{public}.*s" the
// store keeps 1,009 bytes of the text (macOS 27.2: 1,010 and longer end in
// "<…>"). 1,000 stays under it.
inline constexpr std::size_t kUnifiedLogEntryBytes = 1000;
// for_each_utf8_piece needs a cap above 0, and one of 4 or more keeps every
// valid character whole.
static_assert(kUnifiedLogEntryBytes >= 4);

// Calls `emit(piece)` with consecutive pieces of `text`, none empty and each
// at most `max_bytes` long, which together are `text`. A piece ends before
// the byte that starts a UTF-8 character, found at most three bytes back from
// `max_bytes`, so no character is cut in two. Where there is none -- invalid
// UTF-8, or a first character longer than `max_bytes` -- the piece ends at
// `max_bytes`. @pre max_bytes > 0.
template <typename Emit>
void for_each_utf8_piece(std::string_view text, std::size_t max_bytes,
                         Emit&& emit) {
  const auto continues = [&text](std::size_t i) {
    return (static_cast<unsigned char>(text[i]) & 0xC0U) == 0x80U;
  };
  while (text.size() > max_bytes) {
    // Back up from the cut to the lead byte of the character it would split:
    // a UTF-8 character is at most four bytes, so at most three.
    std::size_t end = max_bytes;
    for (int back = 0; back < 3 && end > 0 && continues(end); ++back) --end;
    if (end == 0 || continues(end)) end = max_bytes;
    emit(text.substr(0, end));
    text.remove_prefix(end);
  }
  if (!text.empty()) emit(text);
}

// The built-in sink: warnings and errors to stderr, and on iOS to the unified
// log as well; quieter levels dropped.
void default_sink(LogLevel level, std::string_view source,
                  std::string_view message);

// True while this thread is inside a call to an installed handler.
bool in_log_handler() noexcept;

}  // namespace volumetric_kit::core::detail
