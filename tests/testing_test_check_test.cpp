// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_base's CHECK (test_check.hpp) in a test that is a main() of its
// own: a true condition carries on, a false one returns 1 from the enclosing
// function before anything after it runs, and the condition is evaluated
// once. Checked with plain ifs, so a broken CHECK cannot pass its own test.

#include <cstdio>

#include "volumetric_kit/core/testing/test_check.hpp"

namespace {

int passes(int& reached) {
  CHECK(1 + 1 == 2);
  reached = 1;
  return 0;
}

int fails(int& reached) {
  const int zero = 0;
  CHECK(zero == 1);
  reached = 1;
  return 0;
}

int counts(int& calls) {
  CHECK(++calls == 1);
  return 0;
}

}  // namespace

int main() {
  int reached = 0;
  if (passes(reached) != 0 || reached != 1) {
    std::fputs("a true CHECK did not carry on\n", stderr);
    return 1;
  }
  reached = 0;
  if (fails(reached) != 1 || reached != 0) {
    std::fputs("a false CHECK did not return 1 at once\n", stderr);
    return 1;
  }
  int calls = 0;
  if (counts(calls) != 0 || calls != 1) {
    std::fputs("CHECK did not evaluate its condition once\n", stderr);
    return 1;
  }
  std::puts("test_check: OK");
  return 0;
}
