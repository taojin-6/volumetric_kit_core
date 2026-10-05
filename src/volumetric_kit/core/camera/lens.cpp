// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/camera/lens.hpp"

#include <cmath>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {

Status check_image_size(ImageSize size) {
  if (size.width == 0 || size.height == 0) {
    return Status::invalid_argument("image size has a zero side");
  }
  return {};
}

Status check_intrinsics(const PinholeIntrinsics& intrinsics) {
  const PinholeIntrinsics& k = intrinsics;
  if (!(std::isfinite(k.fx) && k.fx > 0.0 && std::isfinite(k.fy) &&
        k.fy > 0.0 && std::isfinite(k.cx) && std::isfinite(k.cy))) {
    return Status::invalid_argument(
        "intrinsics need finite values and positive focal lengths");
  }
  return {};
}

Status check_distortion(const RationalDistortion& distortion) {
  const RationalDistortion& d = distortion;
  for (const double v : {d.k1, d.k2, d.p1, d.p2, d.k3, d.k4, d.k5, d.k6}) {
    if (!std::isfinite(v)) {
      return Status::invalid_argument("distortion is not finite");
    }
  }
  return {};
}

Result<PinholeIntrinsics> scale_intrinsics(const PinholeIntrinsics& intrinsics,
                                           ImageSize from, ImageSize to) {
  if (!check_image_size(from) || !check_image_size(to)) {
    return Status::invalid_argument("scale_intrinsics: an image size is zero");
  }
  const double sx = static_cast<double>(to.width) / from.width;
  const double sy = static_cast<double>(to.height) / from.height;
  return PinholeIntrinsics{intrinsics.fx * sx, intrinsics.fy * sy,
                           ((intrinsics.cx + 0.5) * sx) - 0.5,
                           ((intrinsics.cy + 0.5) * sy) - 0.5};
}

}  // namespace volumetric_kit::core
