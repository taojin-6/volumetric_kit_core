// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_main (support/test_main.cpp): on Linux a death test's child is
// not dumpable, so a crash reporter never sees its abort, while the test
// process still is.

#include <gtest/gtest.h>

#ifdef __linux__
#include <sys/prctl.h>

#include <cstdlib>
#endif

namespace volumetric_kit::core {
namespace {

#ifdef __linux__

TEST(TestMainDeathTest, AChildDumpsNoCore) {
  EXPECT_EXIT(std::_Exit(prctl(PR_GET_DUMPABLE) == 0 ? 0 : 1),
              testing::ExitedWithCode(0), "");
}

TEST(TestMain, TheTestProcessStillDumps) {
  EXPECT_EQ(prctl(PR_GET_DUMPABLE), 1);
}

#else

TEST(TestMain, ChangesNothingBeyondLinux) {
  GTEST_SKIP() << "only Linux children are made non-dumpable";
}

#endif

}  // namespace
}  // namespace volumetric_kit::core
