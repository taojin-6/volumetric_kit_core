// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file unique_fd.hpp
/// @brief @ref volumetric_kit::core::UniqueFd "UniqueFd", the sole owner of a
///        POSIX file descriptor.

#include <utility>

#include "volumetric_kit/core/base/export.hpp"

namespace volumetric_kit::core {

/// @brief Sole owner of a file descriptor, closing it exactly once unless it
///        is released to an API that takes it.
///
/// Owned, a descriptor is closed on every path, an early return or an
/// exception included. When a call that takes ownership succeeds, the caller
/// calls @ref release; after a failed one it is still owned here.
///
/// @code
/// UniqueFd fd(raw);                     // adopts
/// if (import(fd.get())) fd.release();  // the importer owns it now
/// @endcode
class VKC_BASE_API UniqueFd {
 public:
  /// @brief Construct an empty owner (owns nothing; `valid()` is false).
  UniqueFd() noexcept = default;
  /// @brief Adopt @p fd.
  /// @param fd  An open descriptor, or -1 for none.
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  /// @return The descriptor, still owned here; -1 when empty.
  int get() const noexcept { return fd_; }
  /// @return Whether this owns a descriptor.
  bool valid() const noexcept { return fd_ >= 0; }
  /// @brief Give the descriptor up without closing it, as an import that
  ///        took it needs.
  /// @return The descriptor, now the caller's; -1 when empty.
  int release() noexcept { return std::exchange(fd_, -1); }
  /// @brief Close the descriptor owned, if any, and adopt @p fd.
  /// @param fd  An open descriptor, or -1 for none.
  void reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

}  // namespace volumetric_kit::core
