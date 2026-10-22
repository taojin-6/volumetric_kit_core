// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_main (support/test_main.cpp): on Linux a death test's child is
// not dumpable, so a crash reporter never sees its abort, while the test
// process still is. Built on Linux only.

#include <gtest/gtest.h>
#include <linux/prctl.h>
#include <sys/prctl.h>

#include <cstdlib>

namespace volumetric_kit::core {
namespace {

TEST(TestMainDeathTest, AChildDumpsNoCore) {
  EXPECT_EXIT(std::_Exit(prctl(PR_GET_DUMPABLE) == 0 ? 0 : 1),
              testing::ExitedWithCode(0), "");
}

TEST(TestMain, TheTestProcessStillDumps) {
  EXPECT_EQ(prctl(PR_GET_DUMPABLE), 1);
}

}  // namespace
}  // namespace volumetric_kit::core
