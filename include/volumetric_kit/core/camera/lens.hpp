// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file lens.hpp
/// @brief A camera's lens vocabulary: its image size, pinhole intrinsics, and
///        OpenCV's rational distortion.
///
/// Pixel coordinates follow OpenCV: pixel centres sit at integer coordinates,
/// so a W-pixel-wide image spans [-0.5, W - 0.5]. Normalized coordinates are
/// the pinhole's, `x = (u - cx) / fx`, `y = (v - cy) / fy`.

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/camera/export.hpp"
#include "volumetric_kit/core/camera/geometry.hpp"

namespace volumetric_kit::core {

/// @brief An image's size in pixels.
struct ImageSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

/// @brief Pinhole intrinsics, in pixels of the image they were calibrated at.
struct PinholeIntrinsics {
  double fx = 0.0;  ///< Focal length x.
  double fy = 0.0;  ///< Focal length y.
  double cx = 0.0;  ///< Principal point x.
  double cy = 0.0;  ///< Principal point y.
};

/// @brief OpenCV's rational lens distortion, its eight coefficients in its
///        order; all zero is a pinhole lens.
///
/// The radial factor is `(1 + k1 r^2 + k2 r^4 + k3 r^6) /
/// (1 + k4 r^2 + k5 r^4 + k6 r^6)`. With `k4 = k5 = k6 = 0` it is the
/// five-coefficient Brown-Conrady model, so a Brown-Conrady lens is
/// `RationalDistortion{k1, k2, p1, p2, k3}`.
struct RationalDistortion {
  double k1 = 0.0;  ///< Radial, numerator, r^2.
  double k2 = 0.0;  ///< Radial, numerator, r^4.
  double p1 = 0.0;  ///< Tangential.
  double p2 = 0.0;  ///< Tangential.
  double k3 = 0.0;  ///< Radial, numerator, r^6.
  double k4 = 0.0;  ///< Radial, denominator, r^2.
  double k5 = 0.0;  ///< Radial, denominator, r^4.
  double k6 = 0.0;  ///< Radial, denominator, r^6.
};

/// @return OK if both of @p size's sides are nonzero; else
///         @ref Status::Code::InvalidArgument.
VKC_CAMERA_API Status check_image_size(ImageSize size);

/// @return OK if @p intrinsics are finite with positive focal lengths; else
///         @ref Status::Code::InvalidArgument.
VKC_CAMERA_API Status check_intrinsics(const PinholeIntrinsics& intrinsics);

/// @return OK if every coefficient of @p distortion is finite; else
///         @ref Status::Code::InvalidArgument.
VKC_CAMERA_API Status check_distortion(const RationalDistortion& distortion);

/// @brief The intrinsics of the same camera at another image size.
///
/// The focal length scales with the size, and the principal point about the
/// pixel centres: `c' = (c + 0.5) s - 0.5`, where `s` is the axis's ratio of
/// sizes. A centred principal point stays centred; dropping the half-pixel term
/// would bias every ray by `0.5 (1 - s)` px. Normalized coordinates do not
/// change, so neither does the lens's distortion.
///
/// @code
/// // A 3840x2160 calibration, streamed at 1920x1080.
/// VKC_ASSIGN(PinholeIntrinsics half,
///            scale_intrinsics(k, {3840, 2160}, {1920, 1080}));
/// @endcode
/// @param intrinsics  The intrinsics at @p from.
/// @param from        The size @p intrinsics are in pixels of.
/// @param to          The size to express them at.
/// @return The intrinsics at @p to; or @ref Status::Code::InvalidArgument if
///         a side of either size is zero.
VKC_CAMERA_API Result<PinholeIntrinsics> scale_intrinsics(
    const PinholeIntrinsics& intrinsics, ImageSize from, ImageSize to);

/// @brief Where the lens images a point: its normalized pinhole coordinates
///        to the normalized coordinates it lands at.
///
/// OpenCV's forward model, a rational radial factor and a tangential term,
/// as `cv::projectPoints` applies it. It is inline so a pixel loop pays no
/// call, and a GPU undistortion that mirrors it in a shader can cite it.
/// @param d  The lens.
/// @param p  A point in normalized pinhole coordinates.
/// @return The normalized coordinates @p p is imaged at; not finite where the
///         radial factor's denominator vanishes.
inline Vec2d distort_normalized(const RationalDistortion& d, Vec2d p) noexcept {
  const double r2 = (p.x * p.x) + (p.y * p.y);
  const double r4 = r2 * r2;
  const double r6 = r4 * r2;
  const double radial = (1.0 + (d.k1 * r2) + (d.k2 * r4) + (d.k3 * r6)) /
                        (1.0 + (d.k4 * r2) + (d.k5 * r4) + (d.k6 * r6));
  const double xy = p.x * p.y;
  return {
      (p.x * radial) + (2.0 * d.p1 * xy) + (d.p2 * (r2 + (2.0 * p.x * p.x))),
      (p.y * radial) + (d.p1 * (r2 + (2.0 * p.y * p.y))) + (2.0 * d.p2 * xy)};
}

}  // namespace volumetric_kit::core
