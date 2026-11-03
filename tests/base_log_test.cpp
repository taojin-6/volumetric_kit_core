// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/log.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "log_internal.hpp"

namespace volumetric_kit::core {
namespace {

// Restores the default sink after each test, so an installed handler never
// leaks into another test.
class LogTest : public ::testing::Test {
 protected:
  void TearDown() override { set_log_handler({}); }
};

// Runs in a child process so a missed notification fails in bounded time,
// without leaving a blocked logging thread (and its handler) in the suite.
[[noreturn]] void self_replacement_with_another_call(bool nested) {
  constexpr auto kTimeout = std::chrono::seconds(5);
  std::promise<void> entered;
  std::promise<void> release;
  std::promise<void> replaced;
  auto entered_future = entered.get_future();
  auto release_future = release.get_future();
  auto replaced_future = replaced.get_future();
  std::atomic<bool> replacement_observed{false};
  set_log_handler([&](LogLevel, std::string_view, std::string_view message) {
    if (message == "slow") {
      entered.set_value();
      release_future.wait();
    } else if (message == "replace" && nested) {
      log_message(LogLevel::Info, "test", "nested replace");
    } else if (message == "replace" || message == "nested replace") {
      set_log_handler([&](LogLevel, std::string_view, std::string_view) {
        replacement_observed = true;
      });
      replaced.set_value();
    }
  });

  std::thread slow([] { log_message(LogLevel::Info, "test", "slow"); });
  if (entered_future.wait_for(kTimeout) != std::future_status::ready) {
    std::_Exit(1);
  }
  std::thread replacing([] { log_message(LogLevel::Info, "test", "replace"); });
  // Observing the new handler proves replacement has happened before the
  // old call returns. It must still wait for that call, including when two
  // nested old-handler calls on the replacing thread remain in flight.
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (!replacement_observed) {
    if (std::chrono::steady_clock::now() >= deadline) std::_Exit(2);
    log_message(LogLevel::Info, "test", "probe");
    std::this_thread::yield();
  }
  if (replaced_future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::ready) {
    std::_Exit(3);
  }
  release.set_value();
  if (replaced_future.wait_for(kTimeout) != std::future_status::ready) {
    std::_Exit(4);
  }
  slow.join();
  replacing.join();
  set_log_handler({});
  std::_Exit(0);
}

TEST(LogDeathTest, SelfReplacementWaitsForAnotherThreadsCall) {
  EXPECT_EXIT(self_replacement_with_another_call(false),
              ::testing::ExitedWithCode(0), "");
}

TEST(LogDeathTest, NestedSelfReplacementWaitsForAnotherThreadsCall) {
  EXPECT_EXIT(self_replacement_with_another_call(true),
              ::testing::ExitedWithCode(0), "");
}

TEST_F(LogTest, HandlerReceivesLevelSourceAndMessage) {
  LogLevel level = LogLevel::Debug;
  std::string source;
  std::string message;
  int calls = 0;
  set_log_handler([&](LogLevel l, std::string_view s, std::string_view m) {
    level = l;
    source.assign(s);
    message.assign(m);
    ++calls;
  });

  log_message(LogLevel::Error, "recon", "boom");

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(level, LogLevel::Error);
  EXPECT_EQ(source, "recon");
  EXPECT_EQ(message, "boom");
}

TEST_F(LogTest, EveryLevelReachesAnInstalledHandler) {
  // Filtering by severity is the default sink's job; an installed handler sees
  // every level so the application can route them as it likes.
  std::vector<LogLevel> seen;
  set_log_handler([&](LogLevel l, std::string_view, std::string_view) {
    seen.push_back(l);
  });

  log_message(LogLevel::Debug, "test", "d");
  log_message(LogLevel::Info, "test", "i");
  log_message(LogLevel::Warning, "test", "w");
  log_message(LogLevel::Error, "test", "e");

  const std::vector<LogLevel> want = {LogLevel::Debug, LogLevel::Info,
                                      LogLevel::Warning, LogLevel::Error};
  EXPECT_EQ(seen, want);
}

TEST_F(LogTest, AStatefulHandlerKeepsItsState) {
  // Every call runs the one installed handler object, so a counter or rate
  // limiter captured by value accumulates instead of restarting each call.
  int last_count = 0;
  set_log_handler(
      [n = 0, &last_count](LogLevel, std::string_view,
                           std::string_view) mutable { last_count = ++n; });

  log_message(LogLevel::Info, "test", "a");
  log_message(LogLevel::Info, "test", "b");
  log_message(LogLevel::Info, "test", "c");

  EXPECT_EQ(last_count, 3);
}

TEST_F(LogTest, ResettingTheHandlerStopsDelivery) {
  int calls = 0;
  set_log_handler(
      [&](LogLevel, std::string_view, std::string_view) { ++calls; });
  log_message(LogLevel::Error, "test", "x");
  ASSERT_EQ(calls, 1);

  set_log_handler({});  // back to the built-in stderr sink
  ::testing::internal::CaptureStderr();
  log_message(LogLevel::Error, "test", "y");  // must not reach our handler
  (void)::testing::internal::GetCapturedStderr();
  EXPECT_EQ(calls, 1);
}

TEST_F(LogTest, AHandlerMayLogWithoutDeadlocking) {
  std::vector<std::string> seen;
  set_log_handler([&](LogLevel level, std::string_view, std::string_view m) {
    seen.emplace_back(m);
    if (level == LogLevel::Error) {
      log_message(LogLevel::Info, "test", "nested");
    }
  });

  log_message(LogLevel::Error, "test", "outer");

  const std::vector<std::string> want = {"outer", "nested"};
  EXPECT_EQ(seen, want);
}

TEST_F(LogTest, AReplacedHandlerMayLogFromItsDestructor) {
  // The previous handler is destroyed outside the sink's lock, so a captured
  // object that logs on its way out does not deadlock set_log_handler.
  struct Sink {
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;
    ~Sink() { log_message(LogLevel::Info, "test", "closing"); }
  };
  auto sink = std::make_shared<Sink>();
  set_log_handler([sink](LogLevel, std::string_view, std::string_view) {});
  sink.reset();  // the handler now holds the last reference

  std::vector<std::string> seen;
  set_log_handler([&](LogLevel, std::string_view, std::string_view m) {
    seen.emplace_back(m);
  });

  const std::vector<std::string> want = {"closing"};
  EXPECT_EQ(seen, want);
}

TEST_F(LogTest, SetLogHandlerWaitsForCallsInFlightOnOtherThreads) {
  // Once set_log_handler returns, no other thread is still inside the
  // previous handler, so whatever it references may be destroyed.
  std::promise<void> entered;
  std::atomic<bool> finished{false};
  set_log_handler([&](LogLevel, std::string_view, std::string_view) {
    entered.set_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    finished = true;
  });

  std::thread logger(
      [] { log_message(LogLevel::Info, "test", "slow handler"); });
  entered.get_future().wait();
  set_log_handler({});
  EXPECT_TRUE(finished);
  logger.join();
}

TEST_F(LogTest, ReplacingTheHandlerWhileOtherThreadsLogIsSafe) {
  // Each handler checks that the object it references is still alive; the
  // main thread destroys that object as soon as set_log_handler returns.
  // Run under the sanitizers, a handler outliving its referent would show.
  struct Alive {
    std::atomic<bool> alive{true};
  };
  std::atomic<bool> stop{false};
  std::atomic<int> dead_reads{0};
  constexpr int kLoggers = 4;
  std::vector<std::thread> loggers;
  loggers.reserve(kLoggers);
  for (int t = 0; t < kLoggers; ++t) {
    loggers.emplace_back([&] {
      while (!stop) log_message(LogLevel::Debug, "test", "spin");
    });
  }
  for (int round = 0; round < 50; ++round) {
    auto referent = std::make_unique<Alive>();
    Alive* raw = referent.get();
    set_log_handler(
        [raw, &dead_reads](LogLevel, std::string_view, std::string_view) {
          if (!raw->alive) ++dead_reads;
        });
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    set_log_handler({});
    raw->alive = false;  // no handler that sees `raw` is still running
    referent.reset();
  }
  stop = true;
  for (std::thread& t : loggers) t.join();
  EXPECT_EQ(dead_reads, 0);
}

TEST_F(LogTest, AHandlerMayReplaceItself) {
  // set_log_handler from inside the handler cannot wait for its own caller;
  // it returns, and the outer call finishes on the handler it started with.
  int calls = 0;
  set_log_handler([&](LogLevel, std::string_view, std::string_view) {
    ++calls;
    set_log_handler({});
  });

  log_message(LogLevel::Info, "test", "first");
  ::testing::internal::CaptureStderr();
  log_message(LogLevel::Info, "test", "second");  // default sink: dropped
  EXPECT_EQ(::testing::internal::GetCapturedStderr(), "");

  EXPECT_EQ(calls, 1);
}

TEST_F(LogTest, DefaultSinkPrintsWarningsAndErrorsWithTheirSource) {
  ::testing::internal::CaptureStderr();
  log_message(LogLevel::Debug, "recon", "quiet debug");
  log_message(LogLevel::Info, "recon", "quiet info");
  log_message(LogLevel::Warning, "recon", "loud warning");
  log_message(LogLevel::Error, std::string_view("gfx!", 3),
              std::string_view("loud error!", 10));
  const std::string out = ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(out,
            "[recon warning] loud warning\n"
            "[gfx error] loud error\n");
}

// The default sink's split for the unified log on iOS (log_internal.hpp),
// tested here on the host: the pieces it hands each entry.
std::vector<std::string> pieces(std::string_view text, std::size_t max_bytes) {
  std::vector<std::string> out;
  detail::for_each_utf8_piece(text, max_bytes, [&](std::string_view piece) {
    // Every piece is non-empty, so there are at most text.size() of them: a
    // split that stops advancing fails here at once, not after filling memory.
    if (piece.empty() || out.size() == text.size()) {
      std::fputs("for_each_utf8_piece does not advance\n", stderr);
      std::abort();
    }
    out.emplace_back(piece);
  });
  return out;
}

TEST(LogUnifiedSplit, ATextWithinTheCapIsOnePieceAndNothingIsNone) {
  EXPECT_EQ(pieces("abc", 3), std::vector<std::string>{"abc"});
  EXPECT_EQ(pieces("", 3), std::vector<std::string>{});
}

TEST(LogUnifiedSplit, AsciiIsCutAtTheCap) {
  const std::vector<std::string> want = {"abc", "def", "g"};
  EXPECT_EQ(pieces("abcdefg", 3), want);
}

TEST(LogUnifiedSplit, ACharacterTheCapWouldSplitStartsTheNextPiece) {
  // "\xC3\xA9" is e-acute, two bytes; "\xE2\x82\xAC" the euro sign, three;
  // "\xF0\x9F\x98\x80" an emoji, four. The cut at 4 lands one, two or three
  // bytes inside the character, which then starts the next piece.
  EXPECT_EQ(pieces("abc\xC3\xA9", 4),
            (std::vector<std::string>{"abc", "\xC3\xA9"}));
  EXPECT_EQ(pieces("ab\xE2\x82\xAC", 4),
            (std::vector<std::string>{"ab", "\xE2\x82\xAC"}));
  EXPECT_EQ(pieces("abc\xE2\x82\xAC", 4),
            (std::vector<std::string>{"abc", "\xE2\x82\xAC"}));
  EXPECT_EQ(pieces("a\xF0\x9F\x98\x80", 4),
            (std::vector<std::string>{"a", "\xF0\x9F\x98\x80"}));
  EXPECT_EQ(pieces("x\xF0\x9F\x98\x80y", 5),
            (std::vector<std::string>{"x\xF0\x9F\x98\x80", "y"}));
  // A character ending exactly at the cap stays whole in the first piece.
  EXPECT_EQ(pieces("\xE2\x82\xAC\xE2\x82\xAC", 3),
            (std::vector<std::string>{"\xE2\x82\xAC", "\xE2\x82\xAC"}));
}

TEST(LogUnifiedSplit, InvalidUtf8OrAnOverlongCharacterIsCutAtTheCap) {
  // Five continuation bytes: no character start within three bytes of the
  // cut, so it falls at the cap. A four-byte character under a cap of two
  // cannot fit whole either. Every piece still makes progress.
  EXPECT_EQ(pieces("a\x80\x80\x80\x80\x80", 4),
            (std::vector<std::string>{"a\x80\x80\x80", "\x80\x80"}));
  // Three bytes back from the cut is still a continuation byte, and the 'b'
  // four back is past the longest character: the cut stays at the cap.
  EXPECT_EQ(pieces("ab\x80\x80\x80\x80", 5),
            (std::vector<std::string>{"ab\x80\x80\x80", "\x80"}));
  EXPECT_EQ(pieces("\xF0\x9F\x98\x80", 2),
            (std::vector<std::string>{"\xF0\x9F", "\x98\x80"}));
  EXPECT_EQ(pieces("\xC3\xA9", 1), (std::vector<std::string>{"\xC3", "\xA9"}));
}

TEST(LogUnifiedSplit, ALongLineSplitsIntoEntriesThatKeepEveryByte) {
  // A line the sink composes, over two of the unified log's entries long and
  // made of three-byte characters, so the second cut falls inside one.
  std::string line = "[recon warning] ";
  while (line.size() < (2 * detail::kUnifiedLogEntryBytes) + 500) {
    line += "\xE2\x82\xAC";
  }
  const std::vector<std::string> got =
      pieces(line, detail::kUnifiedLogEntryBytes);
  ASSERT_EQ(got.size(), 3U);
  std::string joined;
  for (std::size_t i = 0; i < got.size(); ++i) {
    const std::string& piece = got[i];
    EXPECT_LE(piece.size(), detail::kUnifiedLogEntryBytes);
    if (i + 1 < got.size()) {
      // A full entry gives back at most the two bytes of a split character.
      EXPECT_GE(piece.size(), detail::kUnifiedLogEntryBytes - 2);
    }
    EXPECT_NE(static_cast<unsigned char>(piece.front()) & 0xC0U, 0x80U);
    joined += piece;
  }
  EXPECT_EQ(joined, line);
}

TEST(LogUnifiedSplit, AnEntryStaysUnderWhatTheStoreKeeps) {
  // The store keeps 1,009 bytes of a "%{public}.*s" argument, not the 1,024
  // os/log.h states for the entry (log_internal.hpp): a piece at the header's
  // figure would lose its tail.
  EXPECT_LE(detail::kUnifiedLogEntryBytes, 1009U);
}

}  // namespace
}  // namespace volumetric_kit::core
