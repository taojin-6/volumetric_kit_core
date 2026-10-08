// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/check.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/log.hpp"

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#include <stdexcept>
#define VKC_TEST_EXCEPTIONS 1
#endif

namespace volumetric_kit::core {
namespace {

TEST(Check, PassingCheckDoesNothing) {
  int evaluated = 0;
  VKC_CHECK(++evaluated == 1, "evaluated once");
  EXPECT_EQ(evaluated, 1);
}

TEST(CheckDeathTest, FailingCheckNamesTheContractAndAborts) {
  EXPECT_DEATH(VKC_CHECK(1 + 1 == 3, "arithmetic holds"),
               "\\[core error\\] contract check failed: arithmetic holds "
               "\\[1 \\+ 1 == 3\\] at .*base_check_test.cpp:[0-9]+");
}

// A failed check reports through the installed sink, so an application that
// routes logs to its crash reporter sees why it aborted.
TEST(CheckDeathTest, FailingCheckReportsThroughTheInstalledHandler) {
  EXPECT_DEATH(
      {
        set_log_handler([](LogLevel level, std::string_view source,
                           std::string_view message) {
          const std::string line =
              std::string(level == LogLevel::Error ? "handler error: "
                                                   : "handler other: ") +
              std::string(source) + ": " + std::string(message) + "\n";
          std::fwrite(line.data(), 1, line.size(), stderr);
        });
        VKC_CHECK(false, "routed");
      },
      "handler error: core: contract check failed: routed");
}

// A check that fails inside the handler skips it and goes straight to stderr:
// through the handler again it would recurse until the stack overflowed, and
// the message would never be written.
TEST(CheckDeathTest, FailingCheckInsideTheHandlerBypassesIt) {
  EXPECT_DEATH(
      {
        set_log_handler([](LogLevel, std::string_view, std::string_view) {
          VKC_CHECK(false, "handler bug");
        });
        log_message(LogLevel::Info, "test", "anything");
      },
      "\\[core error\\] contract check failed: handler bug");
}

// The handler breaks while reporting a failed check: both failures still
// reach stderr, the original first.
TEST(CheckDeathTest, AHandlerFailingOnACheckStillLeavesBothMessages) {
  const auto install_handler_that_fails_on_checks = [] {
    set_log_handler([](LogLevel, std::string_view, std::string_view message) {
      VKC_CHECK(message.find("original") == std::string_view::npos,
                "handler fails on the report");
    });
  };
  EXPECT_DEATH(
      {
        install_handler_that_fails_on_checks();
        VKC_CHECK(false, "original");
      },
      "contract check failed: original");
  EXPECT_DEATH(
      {
        install_handler_that_fails_on_checks();
        VKC_CHECK(false, "original");
      },
      "contract check failed: handler fails on the report");
}

#ifdef VKC_TEST_EXCEPTIONS
// A handler that throws unwinds out of a failed check, which a harness can
// use to test checks without death tests. Each later failed check on the
// thread reports through the handler too, rather than as a failure inside the
// first one's report, whose text the unwinding destroyed.
TEST(Check, AHandlerThatThrowsSeesEveryLaterFailedCheck) {
  struct RestoreDefaultSink {
    RestoreDefaultSink() = default;
    RestoreDefaultSink(const RestoreDefaultSink&) = delete;
    RestoreDefaultSink& operator=(const RestoreDefaultSink&) = delete;
    RestoreDefaultSink(RestoreDefaultSink&&) = delete;
    RestoreDefaultSink& operator=(RestoreDefaultSink&&) = delete;
    ~RestoreDefaultSink() { set_log_handler({}); }
  } const restore;
  set_log_handler([](LogLevel level, std::string_view, std::string_view text) {
    if (level == LogLevel::Error) throw std::runtime_error(std::string(text));
  });
  for (const char* contract : {"first", "second", "third"}) {
    try {
      VKC_CHECK(false, contract);
      ADD_FAILURE() << "the failed check returned";
    } catch (const std::runtime_error& reported) {
      EXPECT_NE(std::string_view(reported.what()).find(contract),
                std::string_view::npos)
          << reported.what();
    }
  }
}
#endif

}  // namespace
}  // namespace volumetric_kit::core
