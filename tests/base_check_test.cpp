// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/check.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/log.hpp"

namespace volumetric_kit::core {
namespace {

TEST(Check, PassingCheckDoesNothing) {
  int evaluated = 0;
  VKC_CHECK(++evaluated == 1, "evaluated once");
  EXPECT_EQ(evaluated, 1);
}

TEST(CheckDeathTest, FailingCheckNamesTheContractAndAborts) {
  EXPECT_DEATH(VKC_CHECK(1 + 1 == 3, "arithmetic holds"),
               "contract check failed: arithmetic holds \\[1 \\+ 1 == 3\\] "
               "at .*base_check_test.cpp:[0-9]+");
}

// A failed check reports through the installed sink, so an application that
// routes logs to its crash reporter sees why it aborted.
TEST(CheckDeathTest, FailingCheckReportsThroughTheInstalledHandler) {
  EXPECT_DEATH(
      {
        set_log_handler([](LogLevel level, std::string_view message) {
          const std::string line =
              std::string(level == LogLevel::Error ? "handler error: "
                                                   : "handler other: ") +
              std::string(message) + "\n";
          std::fwrite(line.data(), 1, line.size(), stderr);
        });
        VKC_CHECK(false, "routed");
      },
      "handler error: contract check failed: routed");
}

}  // namespace
}  // namespace volumetric_kit::core
