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
/// `Status` is deliberately *backend-neutral*: a backend failure carries a
/// tag naming the backend (@ref Status::Backend) and that backend's code as a
/// generic `int64_t` (a `VkResult`, a `CUresult`, ...), never a GPU-API type,
/// so this header -- and the whole base tier -- includes no GPU API. A backend
/// tier adds its own factory and `TRY` macro on top (e.g. the vulkan tier's
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

/// @brief Success, or an error: a domain (@ref Code), for a backend failure
///        the backend and its code, and a human-readable message.
///
/// A default-constructed `Status` is success. Build a failure with a domain
/// factory (@ref invalid_argument, @ref not_found, @ref unsupported,
/// @ref out_of_memory, @ref io_error, @ref numerical) or, for a failed
/// backend call, @ref backend_error, which also carries which backend failed
/// and its own code. Convertible to `bool` (true == success) for terse checks.
/// To add context on the way up, use @ref with_context, which keeps the
/// domain, backend and detail.
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
  /// This is the primary discriminator. @ref backend and @ref detail say more
  /// only when the domain is @ref Code::Backend; for every other domain they
  /// are empty and `0`.
  enum class Code {
    Ok,               ///< Success.
    InvalidArgument,  ///< A malformed or contradictory argument value.
    NotFound,         ///< A named resource or file does not exist.
    Unsupported,      ///< A valid request the device or build cannot satisfy.
    OutOfMemory,      ///< A host or device allocation failed.
    IoError,          ///< A read/write/decode/encode operation failed.
    Numerical,        ///< A solve failed: a singular system, no convergence,
                      ///< or a degenerate or ill-conditioned configuration.
    Backend,          ///< A backend library's call failed; see
                      ///< @ref backend and @ref detail.
  };

  /// @brief The backend whose call failed, for a @ref Code::Backend status.
  ///
  /// Backends number their codes independently, so a code means nothing
  /// without its backend: `2` is `VK_TIMEOUT`, `CUDA_ERROR_OUT_OF_MEMORY` and
  /// `NVJPEG_STATUS_INVALID_PARAMETER`.
  enum class Backend : std::uint8_t {
    Vulkan,        ///< A `VkResult`.
    Cuda,          ///< A CUDA driver `CUresult` or runtime `cudaError_t`.
    NvJpeg,        ///< An `nvjpegStatus_t`.
    Ffmpeg,        ///< An FFmpeg `AVERROR` code.
    VideoToolbox,  ///< An `OSStatus` from VideoToolbox or Core Media.
    Other,         ///< A backend not listed; @ref detail is its own code.
  };

  /// @brief Construct a success status.
  Status() = default;

  /// @brief Build a backend failure (domain @ref Code::Backend).
  /// @param backend  Which backend's call failed; it says how to read
  ///                 @p detail.
  /// @param detail   The backend's code for the failure, widened to `int64_t`
  ///                 so this tier stays free of GPU APIs.
  /// @param message  Human-readable context, e.g. the failing call.
  /// @pre @p detail is not `0`, which is success in every listed backend
  ///      (`VK_SUCCESS`, `CUDA_SUCCESS`, `noErr`, ...): test the call's result
  ///      before building a failure from it. Violating this aborts via
  ///      @ref VKC_CHECK.
  /// @return A non-OK `Status` carrying @p backend, @p detail and @p message.
  ///
  /// @code
  /// if (r != CUDA_SUCCESS)
  ///   return Status::backend_error(Status::Backend::Cuda, r, "cuMemAlloc");
  /// @endcode
  static Status backend_error(Backend backend, std::int64_t detail,
                              std::string message) {
    VKC_CHECK(detail != 0,
              "Status::backend_error needs a failing backend code; 0 is "
              "success in every backend");
    return Status{backend, detail, std::move(message)};
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

  /// @brief Prefix the message with what failed at the caller's level,
  ///        keeping the domain, the backend and its detail.
  /// @param context  What was being done, e.g. the kernel being built.
  /// @return For an error, the same domain, @ref backend and @ref detail with
  ///         the message `"<context>: <message>"`; for success, success.
  ///
  /// @code
  /// if (!s) return std::move(s).with_context(kernel_name);
  /// @endcode
  Status with_context(std::string_view context) const& {
    Status copy = *this;
    return std::move(copy).with_context(context);
  }
  /// @copydoc with_context
  Status with_context(std::string_view context) && {
    if (!ok()) {
      std::string prefixed(context);
      prefixed += ": ";
      prefixed += message_;
      message_ = std::move(prefixed);
    }
    return std::move(*this);
  }

  /// @return `true` if this is a success status.
  bool ok() const noexcept { return domain_ == Code::Ok; }
  /// @return `true` on success (same as @ref ok).
  explicit operator bool() const noexcept { return ok(); }

  /// @return The error domain; @ref Code::Ok exactly when @ref ok.
  Code domain() const noexcept { return domain_; }
  /// @return The backend whose call failed when @ref domain is
  ///         @ref Code::Backend; empty otherwise.
  std::optional<Backend> backend() const noexcept { return backend_; }
  /// @return The backend's code when @ref domain is @ref Code::Backend; `0`
  ///         otherwise.
  std::int64_t detail() const noexcept { return detail_; }
  /// @return The failure's context message; empty when @ref ok.
  const std::string& message() const noexcept { return message_; }

 private:
  // A non-backend domain carries no backend or detail, and a backend failure
  // always carries both: only backend_error() calls the second overload, so a
  // domain factory cannot pair a backend code with another domain.
  Status(Code domain, std::string message)
      : domain_(domain), message_(std::move(message)) {}
  Status(Backend backend, std::int64_t detail, std::string message)
      : domain_(Code::Backend),
        backend_(backend),
        detail_(detail),
        message_(std::move(message)) {}

  Code domain_ = Code::Ok;
  std::optional<Backend> backend_;
  std::int64_t detail_ = 0;
  std::string message_;
};

/// @brief Human-readable name for a @ref Status::Code (e.g. "InvalidArgument").
/// @param code  A domain value.
/// @return A static, never-empty `string_view`.
VKC_BASE_API std::string_view to_string(Status::Code code) noexcept;

/// @brief Human-readable name for a @ref Status::Backend (e.g. "Cuda").
/// @param backend  A backend value.
/// @return A static, never-empty `string_view`.
VKC_BASE_API std::string_view to_string(Status::Backend backend) noexcept;

namespace detail {

/// @brief A call site, captured by a defaulted argument -- C++17's stand-in
///        for C++20's `std::source_location`.
struct SourceLocation {
  const char* file;  ///< Source file of the call.
  int line;          ///< Source line of the call.

  /// @return Where the call that defaulted this argument was made: the
  ///         builtins (GCC and Clang) evaluate at the caller, as
  ///         `source_location` does.
  static constexpr SourceLocation current(
      const char* file_name = __builtin_FILE(),
      int line_number = __builtin_LINE()) noexcept {
    return {file_name, line_number};
  }
};

/// @brief Abort for reading the value of an error @ref Result: names the
///        accessor and the held error, then fails like @ref VKC_CHECK.
/// @param accessor  The accessor misused, e.g. `"Result::value()"`.
/// @param status    The error the Result holds.
/// @param where     The call site to report.
[[noreturn]] VKC_BASE_API void bad_result_access(const char* accessor,
                                                 const Status& status,
                                                 SourceLocation where);

/// @brief Conversions to `T` that compile but are almost never a success
///        value, so @ref Result's converting constructor refuses them: a
///        pointer turning into `bool` (`return "config missing";` from a
///        `Result<bool>` function, meant as an error), and a null pointer
///        turning into a string-like `T` (`std::string(nullptr)` is undefined
///        behaviour).
template <class U, class T>
inline constexpr bool is_error_prone_conversion_v =
    (std::is_same_v<std::remove_cv_t<T>, bool> &&
     (std::is_pointer_v<std::decay_t<U>> ||
      std::is_member_pointer_v<std::decay_t<U>>)) ||
    (std::is_null_pointer_v<std::decay_t<U>> &&
     std::is_convertible_v<const char*, T>);

}  // namespace detail

/// @brief A value of type `T` on success, or a non-OK @ref Status on failure.
/// @tparam T  The success value type; movable, and not `Status` itself.
///
/// Constructs implicitly from anything convertible to `T` (success) or from a
/// `Status` (failure), so a function can `return value;` or
/// `return some_error;` directly. Always check @ref ok (or the `bool`
/// conversion) before reading the value.
///
/// Reading the value of an error Result aborts with a message naming the
/// accessor and the held error; @ref value also names its caller's file and
/// line. The operators cannot -- C++ forbids default arguments on them -- so
/// they report this header's line instead.
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
  /// up the one user-defined conversion an implicit `return` allows. Two
  /// conversions that compile but are almost always bugs are refused (see
  /// `detail::is_error_prone_conversion_v`): a pointer to `bool`, and a null
  /// pointer to a string. Defined here because its constraint would otherwise
  /// have to be restated out-of-line.
  template <class U = T,
            std::enable_if_t<std::is_convertible_v<U&&, T> &&
                                 !std::is_same_v<std::decay_t<U>, Result> &&
                                 !std::is_same_v<std::decay_t<U>, Status> &&
                                 !detail::is_error_prone_conversion_v<U, T>,
                             int> = 0>
  Result(U&& value)  // NOLINT(google-explicit-constructor): ergonomic success
                     // return
      : value_(std::in_place, std::forward<U>(value)) {}

  /// @brief Construct a failure Result.
  /// @param err  The failure; it must be non-OK (checked by @ref VKC_CHECK).
  Result(Status err);  // NOLINT(google-explicit-constructor): ergonomic error
                       // return

  /// @return `true` if this holds a value rather than an error.
  bool ok() const noexcept { return value_.has_value(); }
  /// @return `true` if this holds a value (same as @ref ok).
  explicit operator bool() const noexcept { return ok(); }
  /// @return The status; non-OK exactly when this is an error Result.
  const Status& status() const& noexcept;
  /// @return The status, moved out of this expiring Result.
  Status status() &&;

  /// @brief Access the held value.
  /// @pre @ref ok is true. Calling this on an error Result is a programmer
  ///      error: it aborts via @ref VKC_CHECK (it never throws), so guard with
  ///      @ref ok first.
  /// @param caller  Where the call is made, reported if it aborts; leave it
  ///                defaulted.
  /// @return Reference to the held value.
  T& value(detail::SourceLocation caller = detail::SourceLocation::current()) &;
  /// @copydoc value()
  const T& value(
      detail::SourceLocation caller = detail::SourceLocation::current()) const&;
  /// @brief Move the held value out of an expiring Result.
  /// @pre @ref ok is true; otherwise aborts, as in @ref value.
  /// @param caller  Where the call is made, reported if it aborts; leave it
  ///                defaulted.
  /// @return The value, by value: a reference into an expiring Result would
  ///         dangle once a temporary Result is destroyed, as in
  ///         `for (auto& p : load().value())`.
  T value(detail::SourceLocation caller = detail::SourceLocation::current()) &&;

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
  /// @brief Move the held value out of an expiring Result.
  /// @pre @ref ok is true; otherwise aborts, as in @ref value.
  /// @return The value, by value, for the reason given at `value() &&`.
  T operator*() &&;

 private:
  // The value, or else the failure, aborting (as `accessor`, at `where`) if
  // there is none. A template so the const and non-const accessors share it.
  template <class Self>
  static auto* checked_value(Self& self, const char* accessor,
                             detail::SourceLocation where);

  // One discriminator: whether this is OK *is* whether it holds a value, and
  // ok() reads nothing else. status_ stays the default (OK) Status unless the
  // failure constructor set it, to a non-OK status it checks, so the two can
  // never disagree. (Not a std::variant<T, Status>: GCC 13 at -O2 reports a
  // false -Wmaybe-uninitialized for the Status string in its destructor, and
  // a consumer building with -Werror would inherit that.)
  std::optional<T> value_;
  Status status_;
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
/// `using Pair = std::pair<int, int>;`). On failure the status is moved out,
/// not copied, so propagating through several levels copies no message.
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
#define VKC_ASSIGN_IMPL_(decl, expr, id)                                   \
  auto _vkc_result_##id = (expr);                                          \
  if (!_vkc_result_##id.ok()) return std::move(_vkc_result_##id).status(); \
  decl = std::move(_vkc_result_##id).value()
// NOLINTEND(bugprone-macro-parentheses)

#include "volumetric_kit/core/base/impl/result.hpp"
