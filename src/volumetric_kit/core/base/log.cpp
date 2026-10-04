// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/log.hpp"

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "log_internal.hpp"

namespace volumetric_kit::core {
namespace {

// One installed handler, and how many calls to it are in flight. Every call
// shares this one handler object, so a stateful handler keeps its state, and
// taking a call costs a reference count rather than a std::function copy.
struct Slot {
  explicit Slot(LogHandler h) : handler(std::move(h)) {}
  const LogHandler handler;
  int calls = 0;  // guarded by LogState::mutex
};

// The installed handler and the mutex guarding it. Heap-allocated and never
// destroyed, so the sink outlives every consumer: a static object may log from
// its destructor during process teardown, and a function-local static here
// could lock an already-destroyed mutex (static destruction-order UB).
struct LogState {
  std::mutex mutex;
  // Notified whenever a call to a replaced Slot returns: a setter inside
  // that handler waits for the other threads, not for its own calls.
  std::condition_variable replaced_idle;
  std::shared_ptr<Slot> slot;  // null => fall back to the default stderr sink
};

LogState& state() {
  static auto* s = new LogState();
  return *s;
}

// The handler calls in flight on this thread, innermost first. A handler that
// logs nests a second call, so this is a stack, kept in the callers' frames.
// set_log_handler counts its own thread's calls here: it cannot wait for them.
struct CallFrame {
  const Slot* slot;
  const CallFrame* outer;
};
thread_local const CallFrame* t_innermost_call = nullptr;

int calls_on_this_thread(const Slot* slot) {
  int n = 0;
  for (const CallFrame* f = t_innermost_call; f != nullptr; f = f->outer) {
    if (f->slot == slot) ++n;
  }
  return n;
}

// One handler call: pushes its frame and, on the way out (even by exception),
// pops it and counts the call out, waking a set_log_handler that waits for it.
class InFlightCall {
 public:
  InFlightCall(LogState& state, Slot& slot)
      : state_(state), slot_(slot), frame_{&slot, t_innermost_call} {
    t_innermost_call = &frame_;
  }
  ~InFlightCall() {
    t_innermost_call = frame_.outer;
    const std::scoped_lock lock(state_.mutex);
    --slot_.calls;
    if (state_.slot.get() != &slot_) {
      state_.replaced_idle.notify_all();
    }
  }
  InFlightCall(const InFlightCall&) = delete;
  InFlightCall& operator=(const InFlightCall&) = delete;
  InFlightCall(InFlightCall&&) = delete;
  InFlightCall& operator=(InFlightCall&&) = delete;

 private:
  LogState& state_;
  Slot& slot_;
  CallFrame frame_;
};

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

}  // namespace

namespace detail {

void default_sink(LogLevel level, std::string_view source,
                  std::string_view message) {
  if (level == LogLevel::Warning || level == LogLevel::Error) {
    // Compose the whole line and write it with one fwrite, so concurrent calls
    // do not interleave. The parts are appended by their exact length rather
    // than printf's %.*s: a string_view need not be NUL-terminated, and its
    // size can exceed INT_MAX.
    std::string line = "[";
    line.append(source.data(), source.size());
    line += ' ';
    line += level_name(level);
    line += "] ";
    line.append(message.data(), message.size());
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
}

bool in_log_handler() noexcept { return t_innermost_call != nullptr; }

}  // namespace detail

void set_log_handler(LogHandler handler) {
  std::shared_ptr<Slot> next =
      handler ? std::make_shared<Slot>(std::move(handler)) : nullptr;
  LogState& s = state();
  std::shared_ptr<Slot> previous;
  {
    std::unique_lock lock(s.mutex);
    previous = std::exchange(s.slot, std::move(next));
    if (previous) {
      // Wait out the other threads' calls; this thread's own (we are inside
      // the handler) cannot finish until we return.
      const int own_calls = calls_on_this_thread(previous.get());
      s.replaced_idle.wait(lock, [&] { return previous->calls == own_calls; });
    }
  }
  // `previous` is released here, unlocked: if this was the last reference,
  // the handler's destructor runs now and may itself log.
}

void log_message(LogLevel level, std::string_view source,
                 std::string_view message) {
  // Take the installed slot and count the call in under the lock, then call
  // the handler unlocked: it may log re-entrantly without deadlocking, and
  // set_log_handler waits for the count to drain before it returns.
  LogState& s = state();
  std::shared_ptr<Slot> slot;
  {
    const std::scoped_lock lock(s.mutex);
    slot = s.slot;
    if (slot) ++slot->calls;
  }
  if (!slot) {
    detail::default_sink(level, source, message);
    return;
  }
  const InFlightCall call(s, *slot);
  slot->handler(level, source, message);
  // `call` counts out before `slot` releases its reference (reverse order of
  // construction), so a replaced handler is destroyed only after its last
  // call has popped its frame -- and never under the lock.
}

}  // namespace volumetric_kit::core
