// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file check.hpp
/// @brief Fail-fast contract checks (`VKC_CHECK`) for programmer errors.
///
/// A failed `VKC_CHECK` is a *bug* -- a violated precondition, such as reading
/// the value of an error @ref volumetric_kit::core::Result -- not a runtime
/// condition a caller could recover from; those flow through `Status` /
/// `Result`. On failure it logs at @ref volumetric_kit::core::LogLevel::Error
/// through the log sink (see log.hpp), with source `"core"`, then calls
/// `std::abort()`. It is active in every build, not only debug ones.
///
/// A check that fails inside the installed log handler -- or while that
/// handler is reporting an earlier failure -- skips the handler and writes to
/// stderr directly (the earlier failure first), so a broken handler cannot
/// recurse until the stack overflows and hide the message.
///
/// Mobile consumers build with `-fno-exceptions`, so abort -- not `throw` -- is
/// the portable way to stop on a bug: it raises SIGABRT, which crash reporters
/// (Crashlytics, os_log, Android tombstones) capture, and it never leaves the
/// empty-`optional` / use-after-error undefined behaviour a skipped check
/// would.

#include <string_view>

#include "volumetric_kit/core/base/export.hpp"

namespace volumetric_kit::core::detail {

/// @brief Report a failed @ref VKC_CHECK through the log sink, then abort.
///        Never returns.
/// @param file  Source file (`__FILE__`).
/// @param line  Source line (`__LINE__`).
/// @param expr  The stringified failing condition.
/// @param msg   Human-readable description of the contract.
[[noreturn]] VKC_BASE_API void check_failed(const char* file, int line,
                                            const char* expr,
                                            std::string_view msg);

}  // namespace volumetric_kit::core::detail

/// @brief Abort, after logging, unless @p cond holds. For programmer errors
///        only; recoverable failures return a `Status` or `Result` instead.
/// @param cond  A precondition expression that must hold.
/// @param msg   A description of the contract (anything convertible to
///              `std::string_view`).
///
/// @code
/// VKC_CHECK(index < size, "index out of range");
/// @endcode
#define VKC_CHECK(cond, msg)                                                  \
  do {                                                                        \
    if (!(cond)) {                                                            \
      ::volumetric_kit::core::detail::check_failed(__FILE__, __LINE__, #cond, \
                                                   (msg));                    \
    }                                                                         \
  } while (0)
