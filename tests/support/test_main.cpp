// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_main: GoogleTest's main, as GTest::gtest_main's, except that on
// Linux a forked child -- a death test's, which aborts on purpose -- dumps no
// core. A host whose core_pattern pipes dumps to a crash reporter (Ubuntu's
// apport) otherwise holds each death test about a second. The test process
// itself still dumps as the host says.

#include <gtest/gtest.h>

#ifdef __linux__
#include <linux/prctl.h>
#include <pthread.h>
#include <sys/prctl.h>
#endif

int main(int argc, char** argv) {
#ifdef __linux__
  static_cast<void>(pthread_atfork(nullptr, nullptr, [] {
    static_cast<void>(prctl(PR_SET_DUMPABLE, 0UL));
  }));
#endif
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
