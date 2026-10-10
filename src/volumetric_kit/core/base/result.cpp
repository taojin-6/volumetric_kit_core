// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/result.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "volumetric_kit/core/base/check.hpp"

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

std::string_view to_string(Status::Backend backend) noexcept {
  switch (backend) {
    case Status::Backend::Vulkan:
      return "Vulkan";
    case Status::Backend::Cuda:
      return "Cuda";
    case Status::Backend::NvJpeg:
      return "NvJpeg";
    case Status::Backend::Ffmpeg:
      return "Ffmpeg";
    case Status::Backend::VideoToolbox:
      return "VideoToolbox";
    case Status::Backend::Other:
      return "Other";
  }
  return "Unknown";
}

namespace detail {

void bad_result_access(const char* accessor, const Status& status,
                       SourceLocation where) {
  // "Result::value() on an error Result (Backend Vulkan -4: vkQueueSubmit)":
  // the held error tells one misuse from another even without a symbolicated
  // stack.
  std::string msg = accessor;
  msg += " on an error Result (";
  msg += to_string(status.domain());
  if (const std::optional<Status::Backend> backend = status.backend()) {
    msg += ' ';
    msg += to_string(*backend);
    msg += ' ';
    msg += std::to_string(status.detail());
  }
  msg += ": ";
  msg += status.message();
  msg += ')';
  check_failed(where.file, where.line, "ok()", msg);
}

}  // namespace detail

}  // namespace volumetric_kit::core
