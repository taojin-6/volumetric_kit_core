// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/result.hpp"

#include <string_view>

namespace volumetric_kit::core {

std::string_view to_string(Status::Code code) noexcept {
  switch (code) {
    case Status::Code::Ok:
      return "Ok";
    case Status::Code::InvalidArgument:
      return "InvalidArgument";
    case Status::Code::NotFound:
      return "NotFound";
    case Status::Code::Unsupported:
      return "Unsupported";
    case Status::Code::OutOfMemory:
      return "OutOfMemory";
    case Status::Code::IoError:
      return "IoError";
    case Status::Code::Numerical:
      return "Numerical";
    case Status::Code::Backend:
      return "Backend";
  }
  return "Unknown";
}

}  // namespace volumetric_kit::core
