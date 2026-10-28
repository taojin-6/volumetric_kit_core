// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file test_check.hpp
/// @brief `CHECK`, for a test that is a `main()` of its own, with no test
///        framework and no Vulkan (`volumetric_kit::core_test_base`).
///
/// @code
/// int run() {
///   CHECK(parse("3") == 3);  // on failure: "FAIL <file>:<line>: <cond>"
///   return 0;
/// }
/// int main() { return run(); }
/// @endcode

#include <cstdio>

/// @brief For test functions returning an exit code: return 1 from the
///        enclosing function when @p cond is false, after printing the
///        failing call site and condition to stderr.
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                            \
    }                                                                      \
  } while (0)

/// @def VKC_TEST_SANITIZED
/// @brief Defined under AddressSanitizer or ThreadSanitizer, which slow a
///        host loop many times over and keep freed memory a while.
#ifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define VKC_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define VKC_TEST_SANITIZED 1
#endif
