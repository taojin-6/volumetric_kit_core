// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file impl/result.hpp
/// @brief Out-of-line template definitions for
///        @ref volumetric_kit::core::Result. Included at the end of result.hpp
///        -- not a standalone header; do not include it directly.
///
/// A Result holds a value exactly when it is OK, so the accessors check the
/// optional itself: the same contract as `ok()`, stated where clang-tidy's
/// optional-access analysis can see it.

#include <utility>

#include "volumetric_kit/core/base/check.hpp"

namespace volumetric_kit::core {

template <class T>
Result<T>::Result(Status err) : status_(std::move(err)) {
  VKC_CHECK(!status_.ok(), "Result(Status) requires a non-OK status");
}

template <class T>
T& Result<T>::value() & {
  VKC_CHECK(value_.has_value(), "Result::value() on an error Result");
  return *value_;
}

template <class T>
const T& Result<T>::value() const& {
  VKC_CHECK(value_.has_value(), "Result::value() on an error Result");
  return *value_;
}

template <class T>
T&& Result<T>::value() && {
  VKC_CHECK(value_.has_value(), "Result::value() on an error Result");
  return std::move(*value_);
}

template <class T>
T* Result<T>::operator->() {
  VKC_CHECK(value_.has_value(), "Result::operator-> on an error Result");
  return &*value_;
}

template <class T>
const T* Result<T>::operator->() const {
  VKC_CHECK(value_.has_value(), "Result::operator-> on an error Result");
  return &*value_;
}

template <class T>
T& Result<T>::operator*() & {
  return value();
}

template <class T>
const T& Result<T>::operator*() const& {
  return value();
}

template <class T>
T&& Result<T>::operator*() && {
  return std::move(*this).value();
}

}  // namespace volumetric_kit::core
