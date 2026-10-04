// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file impl/result.hpp
/// @brief Out-of-line template definitions for
///        @ref volumetric_kit::core::Result. Included at the end of result.hpp
///        -- not a standalone header; do not include it directly.

#include <memory>
#include <utility>

#include "volumetric_kit/core/base/check.hpp"

namespace volumetric_kit::core {

template <class T>
Result<T>::Result(Status err) : status_(std::move(err)) {
  VKC_CHECK(!status_.ok(), "Result(Status) requires a non-OK status");
}

template <class T>
const Status& Result<T>::status() const& noexcept {
  return status_;
}

template <class T>
Status Result<T>::status() && {
  return std::move(status_);
}

template <class T>
template <class Self>
auto* Result<T>::checked_value(Self& self, const char* accessor,
                               detail::SourceLocation where) {
  if (!self.value_.has_value()) {
    detail::bad_result_access(accessor, self.status_, where);
  }
  // addressof: the real address, even for a T that overloads unary operator&.
  return std::addressof(*self.value_);
}

template <class T>
T& Result<T>::value(detail::SourceLocation caller) & {
  return *checked_value(*this, "Result::value()", caller);
}

template <class T>
const T& Result<T>::value(detail::SourceLocation caller) const& {
  return *checked_value(*this, "Result::value()", caller);
}

template <class T>
T Result<T>::value(detail::SourceLocation caller) && {
  return std::move(*checked_value(*this, "Result::value()", caller));
}

template <class T>
T* Result<T>::operator->() {
  return checked_value(*this, "Result::operator->",
                       detail::SourceLocation::current());
}

template <class T>
const T* Result<T>::operator->() const {
  return checked_value(*this, "Result::operator->",
                       detail::SourceLocation::current());
}

template <class T>
T& Result<T>::operator*() & {
  return *checked_value(*this, "Result::operator*",
                        detail::SourceLocation::current());
}

template <class T>
const T& Result<T>::operator*() const& {
  return *checked_value(*this, "Result::operator*",
                        detail::SourceLocation::current());
}

template <class T>
T Result<T>::operator*() && {
  return std::move(*checked_value(*this, "Result::operator*",
                                  detail::SourceLocation::current()));
}

}  // namespace volumetric_kit::core
