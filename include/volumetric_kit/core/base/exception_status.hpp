// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file exception_status.hpp
/// @brief The exception being handled, as a `Status`, for a library whose
///        API returns `Status` but calls code that throws.
///
/// A library catches what a standard-library or third-party call throws at
/// its API and returns it with @ref exception_status, so no exception crosses
/// the API (result.hpp).

#include <string_view>

#include "volumetric_kit/core/base/export.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {

/// @brief Turn the exception being handled into a `Status`.
///
/// A `std::bad_alloc` (or a type derived from it) is `OutOfMemory`; any other
/// exception gets @p fallback, the domain the caller knows its failures to
/// be in. When the message itself cannot be allocated, the result is
/// `OutOfMemory` with a message that needs no allocation.
///
/// @param api       What was called, e.g. `"read_file"`; it prefixes the
///                  message.
/// @param fallback  The domain of an exception other than `std::bad_alloc`:
///                  one a domain factory makes, so not `Ok` or `Backend`.
/// @pre Called while an exception is being handled, i.e. inside a `catch`
///      handler, and @p fallback is neither `Ok` nor `Backend`. Violating
///      either aborts via @ref VKC_CHECK.
/// @return `OutOfMemory` with `"<api>: out of memory"` for a `std::bad_alloc`;
///         @p fallback with `"<api>: <what()>"` for another `std::exception`;
///         @p fallback with `"<api>: unknown exception"` for anything else.
///
/// @code
/// Result<Corners> detect(const Image& image) {
///   try {
///     return detect_or_throw(image);
///   } catch (...) {
///     return exception_status("detect", Status::Code::InvalidArgument);
///   }
/// }
/// @endcode
VKC_BASE_API Status exception_status(std::string_view api,
                                     Status::Code fallback) noexcept;

}  // namespace volumetric_kit::core
