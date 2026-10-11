// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/exception_status.hpp"

#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

using Code = Status::Code;

static_assert(noexcept(exception_status(std::declval<std::string_view>(),
                                        Code::IoError)),
              "exception_status reports a failure without throwing one");

// The Status for `thrown`, thrown and caught as a library's API would.
template <class E>
Status status_of(const E& thrown, Code fallback) {
  try {
    throw thrown;
  } catch (...) {
    return exception_status("detect", fallback);
  }
}

TEST(ExceptionStatus, BadAllocIsOutOfMemoryWhateverTheFallback) {
  for (const Code fallback : {Code::InvalidArgument, Code::IoError}) {
    const Status s = status_of(std::bad_alloc(), fallback);
    EXPECT_EQ(s.domain(), Code::OutOfMemory);
    EXPECT_EQ(s.message(), "detect: out of memory");
  }
  // A type derived from it too.
  EXPECT_EQ(status_of(std::bad_array_new_length(), Code::IoError).domain(),
            Code::OutOfMemory);
}

TEST(ExceptionStatus, AStandardExceptionTakesTheFallbackAndItsWhat) {
  for (const Code fallback :
       {Code::InvalidArgument, Code::NotFound, Code::Unsupported,
        Code::OutOfMemory, Code::IoError, Code::Numerical}) {
    const Status s = status_of(std::runtime_error("bad corners"), fallback);
    EXPECT_EQ(s.domain(), fallback) << to_string(fallback);
    EXPECT_EQ(s.message(), "detect: bad corners");
    EXPECT_FALSE(s.backend().has_value());
    EXPECT_EQ(s.detail(), 0);
  }
}

TEST(ExceptionStatus, AnUnknownExceptionTakesTheFallback) {
  const Status s = status_of(42, Code::InvalidArgument);
  EXPECT_EQ(s.domain(), Code::InvalidArgument);
  EXPECT_EQ(s.message(), "detect: unknown exception");
}

TEST(ExceptionStatusDeathTest, OutsideACatchHandlerAborts) {
  EXPECT_DEATH((void)exception_status("detect", Code::IoError),
               "exception_status: call it inside a catch handler");
}

TEST(ExceptionStatusDeathTest, OkOrBackendAsTheFallbackAborts) {
  for (const Code fallback : {Code::Ok, Code::Backend}) {
    EXPECT_DEATH((void)status_of(std::runtime_error("x"), fallback),
                 "the fallback is a domain a factory makes")
        << to_string(fallback);
  }
}

}  // namespace
}  // namespace volumetric_kit::core
