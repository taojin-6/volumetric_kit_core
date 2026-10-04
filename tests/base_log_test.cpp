// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/log.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

// Restores the default sink after each test, so an installed handler never
// leaks into another test.
class LogTest : public ::testing::Test {
 protected:
  void TearDown() override { set_log_handler({}); }
};

TEST_F(LogTest, HandlerReceivesLevelAndMessage) {
  LogLevel level = LogLevel::Debug;
  std::string message;
  int calls = 0;
  set_log_handler([&](LogLevel l, std::string_view m) {
    level = l;
    message.assign(m);
    ++calls;
  });

  log_message(LogLevel::Error, "boom");

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(level, LogLevel::Error);
  EXPECT_EQ(message, "boom");
}

TEST_F(LogTest, EveryLevelReachesAnInstalledHandler) {
  // Filtering by severity is the default sink's job; an installed handler sees
  // every level so the application can route them as it likes.
  std::vector<LogLevel> seen;
  set_log_handler([&](LogLevel l, std::string_view) { seen.push_back(l); });

  log_message(LogLevel::Debug, "d");
  log_message(LogLevel::Info, "i");
  log_message(LogLevel::Warning, "w");
  log_message(LogLevel::Error, "e");

  const std::vector<LogLevel> want = {LogLevel::Debug, LogLevel::Info,
                                      LogLevel::Warning, LogLevel::Error};
  EXPECT_EQ(seen, want);
}

TEST_F(LogTest, ResettingTheHandlerStopsDelivery) {
  int calls = 0;
  set_log_handler([&](LogLevel, std::string_view) { ++calls; });
  log_message(LogLevel::Error, "x");
  ASSERT_EQ(calls, 1);

  set_log_handler({});  // back to the built-in stderr sink
  ::testing::internal::CaptureStderr();
  log_message(LogLevel::Error, "y");  // must not reach our handler
  (void)::testing::internal::GetCapturedStderr();
  EXPECT_EQ(calls, 1);
}

TEST_F(LogTest, AHandlerMayLogWithoutDeadlocking) {
  std::vector<std::string> seen;
  set_log_handler([&](LogLevel level, std::string_view m) {
    seen.emplace_back(m);
    if (level == LogLevel::Error) log_message(LogLevel::Info, "nested");
  });

  log_message(LogLevel::Error, "outer");

  const std::vector<std::string> want = {"outer", "nested"};
  EXPECT_EQ(seen, want);
}

TEST_F(LogTest, DefaultSinkPrintsWarningsAndErrorsOnly) {
  ::testing::internal::CaptureStderr();
  log_message(LogLevel::Debug, "quiet debug");
  log_message(LogLevel::Info, "quiet info");
  log_message(LogLevel::Warning, "loud warning");
  log_message(LogLevel::Error, std::string_view("loud error!", 10));
  const std::string out = ::testing::internal::GetCapturedStderr();

  EXPECT_EQ(out,
            "[volumetric_kit warning] loud warning\n"
            "[volumetric_kit error] loud error\n");
}

}  // namespace
}  // namespace volumetric_kit::core
