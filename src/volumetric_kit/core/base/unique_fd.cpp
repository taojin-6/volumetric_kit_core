// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/unique_fd.hpp"

#include <unistd.h>

namespace volumetric_kit::core {

void UniqueFd::reset(int fd) noexcept {
  if (fd_ >= 0 && fd_ != fd) close(fd_);
  fd_ = fd;
}

}  // namespace volumetric_kit::core
