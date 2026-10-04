// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file result.hpp
/// @brief Exception-free, backend-neutral error handling for the public API.
///
/// No exceptions cross a library boundary: mobile (iOS/Android) consumers
/// build with `-fno-exceptions`, where a throwing API is unusable. Fallible
/// calls therefore report failure by value:
///
/// - @ref Status    -- success, or an error domain (@ref Status::Code) with an
///                     optional backend detail code and a context message.
/// - @ref Result    -- a `T` on success, or a `Status` on failure.
///
/// @ref VKC_TRY removes the check-and-propagate boilerplate for a `Status`
/// expression, and @ref VKC_ASSIGN does the same for a `Result<T>`. Both
/// early-return on failure, so they appear only inside functions that
/// themselves return `Status` or `Result<T>`.
///
/// `Status` is deliberately *backend-neutral*: its detail code is a generic
/// `int64_t` (a `VkResult`, or a `cudaError_t`), never a GPU-API type, so this
/// header -- and the whole base tier -- includes no GPU API. A backend tier
/// adds its own factory and `TRY` macro on top (e.g. the vulkan tier's
/// `vk_error` and `VKC_VK_TRY`).
///
/// Both types are `[[nodiscard]]`: with no exceptions, a dropped `Status` is a
/// silently lost failure, so dropping one is a compiler warning (an error
/// under `-Werror`).
///
/// Misuse -- reading the value of an error `Result` -- is a programmer error,
/// not a runtime one: it fails fast via @ref VKC_CHECK rather than throwing.
///
/// @code
/// Result<Config> r = Config::load(path);
/// if (!r) return r.status();   // propagate the failure to our caller
/// const Config& config = *r;   // safe: guarded by the check above
/// @endcode

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/export.hpp"

namespace volumetric_kit::core {

/// @brief Success, or an error: a domain (@ref Code), an optional backend
///        detail code, and a human-readable message.
///
/// A default-constructed `Status` is success. Build a failure with a domain
/// factory (@ref invalid_argument, @ref not_found, @ref unsupported,
/// @ref out_of_memory, @ref io_error, @ref numerical) or, for a failed
/// GPU-backend call, @ref backend_error, which also carries the backend's own
/// code. Convertible to `bool` (true == success) for terse checks.
///
/// @code
/// Status s = integrate(frame);
/// if (!s) {
///   log_message(LogLevel::Error,
///               std::string(to_string(s.domain())) + ": " + s.message());
///   return s;
/// }
/// @endcode
class [[nodiscard]] Status {
 public:
  /// @brief The kind of failure a non-OK `Status` reports.
  ///
  /// This is the primary discriminator. @ref detail carries a meaningful code
  /// only when the domain is @ref Code::Backend; for every other domain it is
  /// `0`.
  enum class Code {
    Ok,               ///< Success.
    InvalidArgument,  ///< A malformed or contradictory argument value.
    NotFound,         ///< A named resource or file does not exist.
    Unsupported,      ///< A valid request the device or build cannot satisfy.
    OutOfMemory,      ///< A host or device allocation failed.
    IoError,          ///< A read/write/decode/encode operation failed.
    Numerical,        ///< A solve failed: a singular system, no convergence,
                      ///< or a degenerate or ill-conditioned configuration.
    Backend,          ///< A GPU-backend call (Vulkan, or the CUDA
                      ///< accelerator) failed; see @ref detail.
  };

  /// @brief Construct a success status.
  Status() = default;

  /// @brief Build a GPU-backend failure (domain @ref Code::Backend).
  /// @param detail   The backend's code for the failure (a `VkResult`, or a
  ///                 `cudaError_t`), widened to `int64_t` so this tier stays
  ///                 free of GPU APIs.
  /// @param message  Human-readable context, e.g. the failing call.
  /// @return A non-OK `Status` carrying @p detail and @p message.
  static Status backend_error(std::int64_t detail, std::string message) {
    return Status{Code::Backend, detail, std::move(message)};
  }

  /// @brief Build a failure in the named domain, with no backend detail.
  /// @param message  Human-readable context.
  /// @return A non-OK `Status` whose @ref domain is the factory's and whose
  ///         @ref detail is `0`.
  static Status invalid_argument(std::string message) {
    return Status{Code::InvalidArgument, std::move(message)};
  }
  /// @copydoc invalid_argument
  static Status not_found(std::string message) {
    return Status{Code::NotFound, std::move(message)};
  }
  /// @copydoc invalid_argument
  static Status unsupported(std::string message) {
    return Status{Code::Unsupported, std::move(message)};
  }
  /// @copydoc invalid_argument
  static Status out_of_memory(std::string message) {
    return Status{Code::OutOfMemory, std::move(message)};
  }
  /// @copydoc invalid_argument
  static Status io_error(std::string message) {
    return Status{Code::IoError, std::move(message)};
  }
  /// @copydoc invalid_argument
  static Status numerical(std::string message) {
    return Status{Code::Numerical, std::move(message)};
  }

  /// @return `true` if this is a success status.
  bool ok() const noexcept { return domain_ == Code::Ok; }
  /// @return `true` on success (same as @ref ok).
  explicit operator bool() const noexcept { return ok(); }

  /// @return The error domain; @ref Code::Ok exactly when @ref ok.
  Code domain() const noexcept { return domain_; }
  /// @return The backend's code when @ref domain is @ref Code::Backend; `0`
  ///         otherwise.
  std::int64_t detail() const noexcept { return detail_; }
  /// @return The failure's context message; empty when @ref ok.
  const std::string& message() const noexcept { return message_; }

 private:
  // A non-backend domain carries no detail: this overload fixes detail_ at 0,
  // so a domain factory cannot pair a backend code with another domain. Only
  // backend_error() passes a detail.
  Status(Code domain, std::string message)
      : domain_(domain), message_(std::move(message)) {}
  Status(Code domain, std::int64_t detail, std::string message)
      : domain_(domain), detail_(detail), message_(std::move(message)) {}

  Code domain_ = Code::Ok;
  std::int64_t detail_ = 0;
  std::string message_;
};

/// @brief Human-readable name for a @ref Status::Code (e.g. "InvalidArgument").
/// @param code  A domain value.
/// @return A static, never-empty `string_view`.
VKC_BASE_API std::string_view to_string(Status::Code code) noexcept;

/// @brief A value of type `T` on success, or a non-OK @ref Status on failure.
/// @tparam T  The success value type; movable, and not `Status` itself.
///
/// Constructs implicitly from anything convertible to `T` (success) or from a
/// `Status` (failure), so a function can `return value;` or
/// `return some_error;` directly. Always check @ref ok (or the `bool`
/// conversion) before reading the value.
///
/// @code
/// Result<std::size_t> parse_count(std::string_view text) {
///   if (text.empty()) return Status::invalid_argument("empty count");
///   return text.size();  // implicit success
/// }
/// @endcode
template <class T>
class [[nodiscard]] Result {
  // Result<Status> is ill-formed: its two implicit constructors would collapse
  // into one. A fallible operation with no value returns Status directly.
  static_assert(!std::is_same_v<std::decay_t<T>, Status>,
                "Result<Status> is ill-formed; return Status directly for a "
                "fallible operation with no value");

 public:
  /// @brief Construct a success Result from anything convertible to `T`.
  /// @param value  The success value: a `T`, or e.g. a string literal for a
  ///               `Result<std::string>`, or `std::nullopt` for a
  ///               `Result<std::optional<U>>`.
  ///
  /// A template rather than `Result(T)`, so the conversion to `T` does not use
  /// up the one user-defined conversion an implicit `return` allows. Defined
  /// here because its constraint would otherwise have to be restated
  /// out-of-line.
  template <class U = T,
            std::enable_if_t<std::is_convertible_v<U&&, T> &&
                                 !std::is_same_v<std::decay_t<U>, Result> &&
                                 !std::is_same_v<std::decay_t<U>, Status>,
                             int> = 0>
  Result(U&& value)  // NOLINT(google-explicit-constructor): ergonomic success
                     // return
      : value_(std::in_place, std::forward<U>(value)) {}

  /// @brief Construct a failure Result.
  /// @param err  The failure; it must be non-OK (checked by @ref VKC_CHECK).
  Result(Status err);  // NOLINT(google-explicit-constructor): ergonomic error
                       // return

  /// @return `true` if this holds a value rather than an error.
  bool ok() const noexcept { return status_.ok(); }
  /// @return `true` if this holds a value (same as @ref ok).
  explicit operator bool() const noexcept { return ok(); }
  /// @return The status; non-OK exactly when this is an error Result.
  const Status& status() const noexcept { return status_; }

  /// @brief Access the held value.
  /// @pre @ref ok is true. Calling this on an error Result is a programmer
  ///      error: it aborts via @ref VKC_CHECK (it never throws), so guard with
  ///      @ref ok first.
  /// @return Reference to the held value.
  T& value() &;
  /// @copydoc value()
  const T& value() const&;
  /// @copydoc value()
  T&& value() &&;

  /// @brief Pointer and reference access to the held value.
  /// @pre @ref ok is true; otherwise aborts, as in @ref value.
  /// @return A pointer to the held value.
  T* operator->();
  /// @copydoc operator->()
  const T* operator->() const;
  /// @brief Dereference to the held value.
  /// @pre @ref ok is true; otherwise aborts, as in @ref value.
  /// @return Reference to the held value.
  T& operator*() &;
  /// @copydoc operator*()
  const T& operator*() const&;
  /// @copydoc operator*()
  T&& operator*() &&;

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace volumetric_kit::core

/// @brief Evaluate a `Status` expression and early-return it if not OK.
/// @param expr  An expression yielding a `Status`.
///
/// Usable only inside a function returning `Status` or `Result<T>`: the early
/// `return` carries the failure outward.
///
/// @code
/// Status init() {
///   VKC_TRY(allocate());  // returns the error if this fails
///   return {};            // success
/// }
/// @endcode
#define VKC_TRY(expr)                                    \
  do {                                                   \
    ::volumetric_kit::core::Status _vkc_status = (expr); \
    if (!_vkc_status.ok()) return _vkc_status;           \
  } while (0)

/// @brief Evaluate a `Result<T>` expression, early-return its `Status` on
///        failure, and otherwise move the value into @p decl.
/// @param decl  A variable declaration (e.g. `Config config`) bound to the
///              unwrapped value on success.
/// @param expr  An expression yielding a `Result<T>`.
///
/// The `Result<T>` analogue of @ref VKC_TRY, usable only inside a function
/// returning `Status` or `Result<U>`. Because it declares @p decl in the
/// enclosing scope, it expands to a statement sequence (not a
/// `do { } while`), so it is not a single statement: never use it as the
/// unbraced body of an `if`/`for`/`while`. The hidden temporary is keyed on
/// `__COUNTER__` (not `__LINE__`), so several `VKC_ASSIGN`s in one scope, even
/// on one line, never collide. @p decl is a single macro argument, so a type
/// with a top-level comma needs an alias first (e.g.
/// `using Pair = std::pair<int, int>;`).
///
/// @code
/// Result<Report> analyze(const std::string& path) {
///   VKC_ASSIGN(Config config, Config::load(path));
///   return build_report(config);  // `config` holds the value here
/// }
/// @endcode
#define VKC_ASSIGN(decl, expr) VKC_ASSIGN_(decl, expr, __COUNTER__)
#define VKC_ASSIGN_(decl, expr, id) VKC_ASSIGN_IMPL_(decl, expr, id)
// `decl` is a declaration (`const int n`), which parentheses would break.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define VKC_ASSIGN_IMPL_(decl, expr, id)                        \
  auto _vkc_result_##id = (expr);                               \
  if (!_vkc_result_##id.ok()) return _vkc_result_##id.status(); \
  decl = std::move(_vkc_result_##id).value()
// NOLINTEND(bugprone-macro-parentheses)

#include "volumetric_kit/core/base/impl/result.hpp"
