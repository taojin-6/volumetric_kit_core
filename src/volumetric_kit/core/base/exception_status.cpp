// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/exception_status.hpp"

#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

// "<api>: <what>".
std::string prefixed(std::string_view api, std::string_view what) {
  std::string message(api);
  message += ": ";
  message += what;
  return message;
}

// A Status in `domain`, which exception_status checked on entry is one a
// factory makes.
Status in_domain(Status::Code domain, std::string message) {
  switch (domain) {
    case Status::Code::InvalidArgument:
      return Status::invalid_argument(std::move(message));
    case Status::Code::NotFound:
      return Status::not_found(std::move(message));
    case Status::Code::Unsupported:
      return Status::unsupported(std::move(message));
    case Status::Code::OutOfMemory:
      return Status::out_of_memory(std::move(message));
    case Status::Code::Numerical:
      return Status::numerical(std::move(message));
    case Status::Code::IoError:
    case Status::Code::Ok:
    case Status::Code::Backend:
      break;
  }
  return Status::io_error(std::move(message));
}

}  // namespace

// Only a failed precondition check can leave, through a log handler that
// throws; terminating then ends the program as the check's abort would.
// NOLINTNEXTLINE(bugprone-exception-escape)
Status exception_status(std::string_view api, Status::Code fallback) noexcept {
  VKC_CHECK(std::current_exception() != nullptr,
            "exception_status: call it inside a catch handler");
  const bool factory_domain =
      fallback != Status::Code::Ok && fallback != Status::Code::Backend;
  VKC_CHECK(factory_domain,
            "exception_status: the fallback is a domain a factory makes, not "
            "Ok or Backend");
  try {
    try {
      throw;
    } catch (const std::bad_alloc&) {
      return Status::out_of_memory(prefixed(api, "out of memory"));
    } catch (const std::exception& e) {
      return in_domain(fallback, prefixed(api, e.what()));
    } catch (...) {
      return in_domain(fallback, prefixed(api, "unknown exception"));
    }
  } catch (...) {
    // Building the message threw, which only an allocation can: short enough
    // for the small-string buffer, this message allocates nothing.
    return Status::out_of_memory("out of memory");
  }
}

}  // namespace volumetric_kit::core
