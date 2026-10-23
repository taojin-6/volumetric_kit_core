// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/stage_metrics.hpp"

#include <atomic>
#include <cstdint>

#include "volumetric_kit/core/base/check.hpp"

namespace volumetric_kit::core {
namespace {

// Each thread's open scopes, innermost first through `outer`. Defined here,
// not inline in the header: one stack per thread for the whole process,
// whichever library opens a scope or waits.
thread_local detail::ScopeFrame* innermost = nullptr;
// The next serial, for the whole process: a tag recorded or submitted on a
// thread other than its scope's then matches none of that thread's scopes.
std::atomic<std::uint64_t> next_serial{1};

}  // namespace

namespace detail {

void push_scope(ScopeFrame* frame) noexcept {
  frame->outer = innermost;
  frame->serial = next_serial.fetch_add(1, std::memory_order_relaxed);
  innermost = frame;
}

void pop_scope(ScopeFrame* frame) noexcept {
  // Scopes are RAII and close innermost first, so this is the top; a frame
  // closed out of order is unlinked where it is.
  ScopeFrame** at = &innermost;
  while (*at != nullptr && *at != frame) at = &(*at)->outer;
  // Not on this thread's stack: it opened on another, whose stack would keep
  // it once freed.
  VKC_CHECK(*at == frame,
            "a scope closed on a thread other than the one it opened on");
  *at = frame->outer;
  frame->outer = nullptr;
}

ScopeFrame* innermost_scope() noexcept { return innermost; }

}  // namespace detail

void note_blocked(std::int64_t ns) noexcept {
  if (ns <= 0) return;
  for (detail::ScopeFrame* frame = innermost; frame != nullptr;
       frame = frame->outer) {
    // A wait scope's own span is the wait: what blocks inside it reaches the
    // scopes around it once, when it closes.
    if (frame->wait) return;
    frame->blocked_ns += ns;
  }
}

}  // namespace volumetric_kit::core
