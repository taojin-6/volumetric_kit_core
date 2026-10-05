// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file geometry.hpp
/// @brief The camera tier's geometry: double-precision vectors, a 3x3 matrix,
///        rigid transforms, and Rodrigues vectors.
///
/// Plain aggregates with no math library behind them (DECISIONS.md, "The
/// camera tier"). Calibration needs double precision, and a math library in
/// these headers would reach every consumer that links the tier. A consumer
/// converts at its boundary: `Eigen::Map` over @ref Mat3d::m (row-major), or a
/// cast into its GPU's float layout.

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/camera/export.hpp"

namespace volumetric_kit::core {

/// @brief A 2D point or vector.
struct Vec2d {
  double x = 0.0;
  double y = 0.0;
};

/// @brief A 3D point or vector.
struct Vec3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

/// @brief A 3x3 matrix, row-major (`m[row][column]`); the identity by default.
struct Mat3d {
  double m[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
};

/// @brief A rigid transform, `p -> rotation * p + translation`; the identity by
///        default.
///
/// The camera tier names a transform by what it maps: `camera_to_world` takes
/// a point in a camera's frame to the world's.
///
/// @code
/// RigidTransform camera_to_world;
/// camera_to_world.translation = {0.0, 0.0, 2.0};
/// const Vec3d ahead = camera_to_world * Vec3d{0.0, 0.0, 1.0};  // (0, 0, 3)
/// const RigidTransform world_to_camera = inverse(camera_to_world);
/// @endcode
struct RigidTransform {
  Mat3d rotation;     ///< A proper rotation (@ref check_rigid).
  Vec3d translation;  ///< Applied after the rotation.
};

/// @return The product `a * v`.
VKC_CAMERA_API Vec3d operator*(const Mat3d& a, const Vec3d& v) noexcept;

/// @return The product `a * b`.
VKC_CAMERA_API Mat3d operator*(const Mat3d& a, const Mat3d& b) noexcept;

/// @return The transpose of @p a, which inverts a rotation.
VKC_CAMERA_API Mat3d transpose(const Mat3d& a) noexcept;

/// @return @p p mapped by @p transform: `rotation * p + translation`.
VKC_CAMERA_API Vec3d operator*(const RigidTransform& transform,
                               const Vec3d& p) noexcept;

/// @return The transform that applies @p b, then @p a.
VKC_CAMERA_API RigidTransform operator*(const RigidTransform& a,
                                        const RigidTransform& b) noexcept;

/// @brief The inverse of a rigid transform: `R^T`, `-R^T t`.
/// @pre @p transform's rotation is a rotation (@ref check_rigid); the
///      transpose inverts nothing else.
/// @return The transform that undoes @p transform.
VKC_CAMERA_API RigidTransform inverse(const RigidTransform& transform) noexcept;

/// @brief Whether @p transform is a rigid transform: finite, with a proper
///        rotation.
///
/// The rotation's rows must be orthonormal to within 1e-4, which refuses a
/// matrix that is not a rotation without refusing one rounded through float
/// on its way here, and its determinant positive, which refuses a reflection.
/// @param transform  The transform to check.
/// @return OK; or @ref Status::Code::InvalidArgument naming what is wrong.
VKC_CAMERA_API Status check_rigid(const RigidTransform& transform);

/// @brief The rotation a Rodrigues vector describes: about its direction, by
///        its length in radians.
///
/// Below 1e-12 rad it is the first-order `I + [r]x`, where the axis is
/// undefined.
/// @param rvec  A rotation vector, as OpenCV's `cv::Rodrigues` takes.
/// @return The rotation matrix.
VKC_CAMERA_API Mat3d rotation_from_rodrigues(const Vec3d& rvec) noexcept;

/// @brief The Rodrigues vector of a rotation, its angle in [0, pi].
///
/// The angle comes from `atan2` rather than `acos`, which turns round-off in
/// the matrix into angle error as the angle nears 0 or pi. Past pi/2 the axis
/// comes from the matrix's symmetric part, as `sin(angle)` vanishes toward pi.
/// At exactly pi the axis's sign is arbitrary, as it is for OpenCV.
/// @pre @p rotation is a rotation (@ref check_rigid).
/// @param rotation  The rotation.
/// @return The rotation vector, which @ref rotation_from_rodrigues maps back to
///         @p rotation.
VKC_CAMERA_API Vec3d rodrigues_from_rotation(const Mat3d& rotation) noexcept;

}  // namespace volumetric_kit::core
