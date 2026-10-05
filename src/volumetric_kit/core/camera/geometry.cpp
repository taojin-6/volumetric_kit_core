// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/camera/geometry.hpp"

#include <algorithm>
#include <cmath>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {

namespace {

constexpr double kPi = 3.14159265358979323846;
// How far a rotation's rows may be from orthonormal: refuses a matrix that is
// not a rotation, but not one rounded through float on its way here.
constexpr double kOrthonormalTolerance = 1e-4;

}  // namespace

Vec3d operator*(const Mat3d& a, const Vec3d& v) noexcept {
  return {(a.m[0][0] * v.x) + (a.m[0][1] * v.y) + (a.m[0][2] * v.z),
          (a.m[1][0] * v.x) + (a.m[1][1] * v.y) + (a.m[1][2] * v.z),
          (a.m[2][0] * v.x) + (a.m[2][1] * v.y) + (a.m[2][2] * v.z)};
}

Mat3d operator*(const Mat3d& a, const Mat3d& b) noexcept {
  Mat3d out;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      out.m[i][j] = (a.m[i][0] * b.m[0][j]) + (a.m[i][1] * b.m[1][j]) +
                    (a.m[i][2] * b.m[2][j]);
    }
  }
  return out;
}

Mat3d transpose(const Mat3d& a) noexcept {
  Mat3d out;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) out.m[i][j] = a.m[j][i];
  }
  return out;
}

Vec3d operator*(const RigidTransform& transform, const Vec3d& p) noexcept {
  const Vec3d r = transform.rotation * p;
  return {r.x + transform.translation.x, r.y + transform.translation.y,
          r.z + transform.translation.z};
}

RigidTransform operator*(const RigidTransform& a,
                         const RigidTransform& b) noexcept {
  return {a.rotation * b.rotation, a * b.translation};
}

RigidTransform inverse(const RigidTransform& transform) noexcept {
  const Mat3d rt = transpose(transform.rotation);
  const Vec3d t = rt * transform.translation;
  return {rt, {-t.x, -t.y, -t.z}};
}

Status check_rigid(const RigidTransform& transform) {
  const Mat3d& r = transform.rotation;
  const Vec3d& t = transform.translation;
  for (const auto& row : r.m) {
    for (const double v : row) {
      if (!std::isfinite(v)) {
        return Status::invalid_argument("rotation is not finite");
      }
    }
  }
  if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z)) {
    return Status::invalid_argument("translation is not finite");
  }
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      const double dot = (r.m[a][0] * r.m[b][0]) + (r.m[a][1] * r.m[b][1]) +
                         (r.m[a][2] * r.m[b][2]);
      if (std::fabs(dot - (a == b ? 1.0 : 0.0)) > kOrthonormalTolerance) {
        return Status::invalid_argument("rotation is not orthonormal");
      }
    }
  }
  const double det =
      (r.m[0][0] * ((r.m[1][1] * r.m[2][2]) - (r.m[1][2] * r.m[2][1]))) -
      (r.m[0][1] * ((r.m[1][0] * r.m[2][2]) - (r.m[1][2] * r.m[2][0]))) +
      (r.m[0][2] * ((r.m[1][0] * r.m[2][1]) - (r.m[1][1] * r.m[2][0])));
  if (!(det > 0.0)) return Status::invalid_argument("rotation is a reflection");
  return {};
}

Mat3d rotation_from_rodrigues(const Vec3d& rvec) noexcept {
  const double theta =
      std::sqrt((rvec.x * rvec.x) + (rvec.y * rvec.y) + (rvec.z * rvec.z));
  if (theta < 1e-12) {  // first order: I + [r]x
    return {{{1.0, -rvec.z, rvec.y},
             {rvec.z, 1.0, -rvec.x},
             {-rvec.y, rvec.x, 1.0}}};
  }
  const double k[3] = {rvec.x / theta, rvec.y / theta, rvec.z / theta};
  const double c = std::cos(theta);
  const double s = std::sin(theta);
  const double t = 1.0 - c;
  const double kx[3][3] = {
      {0.0, -k[2], k[1]}, {k[2], 0.0, -k[0]}, {-k[1], k[0], 0.0}};
  Mat3d r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.m[i][j] = (i == j ? c : 0.0) + (t * k[i] * k[j]) + (s * kx[i][j]);
    }
  }
  return r;
}

Vec3d rodrigues_from_rotation(const Mat3d& rotation) noexcept {
  const auto& r = rotation.m;
  const double vee[3] = {r[2][1] - r[1][2], r[0][2] - r[2][0],
                         r[1][0] - r[0][1]};
  const double cos_theta =
      std::clamp((r[0][0] + r[1][1] + r[2][2] - 1.0) / 2.0, -1.0, 1.0);
  const double sin_theta =
      0.5 *
      std::sqrt((vee[0] * vee[0]) + (vee[1] * vee[1]) + (vee[2] * vee[2]));
  const double theta = std::atan2(sin_theta, cos_theta);
  if (theta < 1e-6) {  // first order
    return {0.5 * vee[0], 0.5 * vee[1], 0.5 * vee[2]};
  }
  if (theta < kPi / 2.0) {
    const double scale = theta / (2.0 * sin_theta);
    return {scale * vee[0], scale * vee[1], scale * vee[2]};
  }
  // Past pi/2, sin(theta) shrinks toward pi, so take the axis from the
  // symmetric part, (R + R^T)/2 = cos(theta) I + (1 - cos(theta)) k k^T, off
  // its largest diagonal, and only its sign from vee.
  double kk[3][3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      kk[i][j] = (((r[i][j] + r[j][i]) / 2.0) - (i == j ? cos_theta : 0.0)) /
                 (1.0 - cos_theta);
    }
  }
  int a = 0;
  for (int i = 1; i < 3; ++i) {
    if (kk[i][i] > kk[a][a]) a = i;
  }
  double k[3];
  k[a] = std::sqrt(std::max(0.0, kk[a][a]));
  for (int i = 0; i < 3; ++i) {
    if (i != a) k[i] = kk[a][i] / k[a];
  }
  const double dot = (k[0] * vee[0]) + (k[1] * vee[1]) + (k[2] * vee[2]);
  const double sign = dot < 0.0 ? -1.0 : 1.0;
  return {sign * theta * k[0], sign * theta * k[1], sign * theta * k[2]};
}

}  // namespace volumetric_kit::core
