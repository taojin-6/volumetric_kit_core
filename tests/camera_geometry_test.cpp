// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/camera/geometry.hpp"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

constexpr double kPi = 3.14159265358979323846;

void expect_near(const Vec3d& a, const Vec3d& b, double eps) {
  EXPECT_NEAR(a.x, b.x, eps);
  EXPECT_NEAR(a.y, b.y, eps);
  EXPECT_NEAR(a.z, b.z, eps);
}

void expect_near(const Mat3d& a, const Mat3d& b, double eps) {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      EXPECT_NEAR(a.m[i][j], b.m[i][j], eps) << "at " << i << ", " << j;
    }
  }
}

// A rotation of `angle` about `axis`, from Rodrigues' formula's vector form.
Vec3d axis_angle(Vec3d axis, double angle) {
  const double n =
      std::sqrt((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z));
  return {axis.x / n * angle, axis.y / n * angle, axis.z / n * angle};
}

TEST(Geometry, DefaultsAreIdentities) {
  const RigidTransform t;
  expect_near(t * Vec3d{1.0, 2.0, 3.0}, {1.0, 2.0, 3.0}, 0.0);
  EXPECT_TRUE(check_rigid(t).ok());
}

TEST(Geometry, MatrixProductsAndTranspose) {
  const Mat3d a = {{{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}, {7.0, 8.0, 10.0}}};
  expect_near(a * Vec3d{1.0, 0.0, -1.0}, {-2.0, -2.0, -3.0}, 0.0);
  const Mat3d at = transpose(a);
  EXPECT_EQ(at.m[0][2], 7.0);
  EXPECT_EQ(at.m[2][0], 3.0);
  expect_near(a * Mat3d{}, a, 0.0);
  const Mat3d aat = a * at;
  EXPECT_EQ(aat.m[0][0], 14.0);  // row 0 . row 0
  EXPECT_EQ(aat.m[0][1], 32.0);  // row 0 . row 1
  EXPECT_EQ(aat.m[1][0], 32.0);
}

TEST(Geometry, RodriguesTurnsAboutItsAxis) {
  // A quarter turn about +y takes +x to -z, as OpenCV's cv::Rodrigues does.
  const Mat3d r = rotation_from_rodrigues({0.0, kPi / 2.0, 0.0});
  expect_near(r * Vec3d{1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, 1e-15);
  expect_near(r * Vec3d{0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, 1e-15);
}

TEST(Geometry, RodriguesRoundTripsAtItsEdgeCases) {
  // No rotation, a tiny one, a general one, and turns of pi and just under
  // it, where sin(angle) vanishes and the axis must come from elsewhere.
  const Vec3d cases[] = {
      {0.0, 0.0, 0.0},
      axis_angle({0.3, 0.5, 0.8}, 1e-7),
      axis_angle({0.3, -0.8, 0.52}, 0.7),
      axis_angle({0.2, 0.9, -0.4}, 2.0),
      axis_angle({1.0, 0.0, 0.0}, kPi),
      axis_angle({1.0, 1.0, 0.0}, kPi),
      axis_angle({0.2, 0.9, -0.4}, 3.1415),
  };
  for (const Vec3d& rvec : cases) {
    const Mat3d r = rotation_from_rodrigues(rvec);
    EXPECT_TRUE(check_rigid({r, {}}).ok());
    const Vec3d back = rodrigues_from_rotation(r);
    // At exactly pi the axis's sign is free; the rotation is what must match.
    expect_near(rotation_from_rodrigues(back), r, 1e-12);
    const double angle =
        std::sqrt((rvec.x * rvec.x) + (rvec.y * rvec.y) + (rvec.z * rvec.z));
    if (angle < 3.0) expect_near(back, rvec, 1e-12);
  }
}

TEST(Geometry, RodriguesFindsTheAxisOfAnExactHalfTurn) {
  // A half turn written exactly, as a calibration might hand one over: the
  // matrix is symmetric, so the skew part that carries the axis below pi is
  // zero, and the axis must come from the symmetric part.
  const Mat3d about_x = {{{1.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}}};
  const Mat3d about_xy = {{{0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}}};
  for (const Mat3d& r : {about_x, about_xy}) {
    const Vec3d back = rodrigues_from_rotation(r);
    EXPECT_NEAR(
        std::sqrt((back.x * back.x) + (back.y * back.y) + (back.z * back.z)),
        kPi, 1e-15);
    expect_near(rotation_from_rodrigues(back), r, 1e-15);
  }

  // Just short of a half turn, rounded through float: the skew part's true
  // size (2 sin(1e-6)) is near the rounding's, so the axis it gives is noise,
  // and only the symmetric part recovers the rotation to float's precision.
  Mat3d rounded =
      rotation_from_rodrigues(axis_angle({0.2, 0.9, -0.4}, kPi - 1e-6));
  for (auto& row : rounded.m) {
    for (double& v : row) v = static_cast<float>(v);
  }
  expect_near(rotation_from_rodrigues(rodrigues_from_rotation(rounded)),
              rounded, 1e-6);
}

TEST(Geometry, TransformsComposeAndInvert) {
  const RigidTransform a{
      rotation_from_rodrigues(axis_angle({1.0, 2.0, 3.0}, 0.4)),
      {0.5, -1.0, 2.0}};
  const RigidTransform b{
      rotation_from_rodrigues(axis_angle({-2.0, 0.5, 1.0}, 1.3)),
      {-0.25, 0.0, 4.0}};
  const Vec3d p{0.3, -0.7, 1.9};
  // a * b applies b first.
  expect_near((a * b) * p, a * (b * p), 1e-14);
  expect_near(inverse(a) * (a * p), p, 1e-14);
  const RigidTransform identity = a * inverse(a);
  expect_near(identity.rotation, Mat3d{}, 1e-15);
  expect_near(identity.translation, {}, 1e-15);
}

TEST(Geometry, CheckRigidRefusesWhatIsNotARotation) {
  RigidTransform t;
  t.translation.y = std::numeric_limits<double>::infinity();
  EXPECT_EQ(check_rigid(t).domain(), Status::Code::InvalidArgument);

  t = {};
  t.rotation.m[1][1] = std::nan("");
  EXPECT_EQ(check_rigid(t).domain(), Status::Code::InvalidArgument);

  t = {};
  t.rotation.m[0][0] = 2.0;  // a scale
  EXPECT_EQ(check_rigid(t).domain(), Status::Code::InvalidArgument);

  t = {};
  t.rotation.m[2][2] = -1.0;  // a reflection: orthonormal, determinant -1
  EXPECT_EQ(check_rigid(t).domain(), Status::Code::InvalidArgument);

  // A rotation rounded through float on its way here passes.
  t.rotation = rotation_from_rodrigues(axis_angle({0.3, -0.8, 0.52}, 0.7));
  for (auto& row : t.rotation.m) {
    for (double& v : row) v = static_cast<float>(v);
  }
  EXPECT_TRUE(check_rigid(t).ok());
}

}  // namespace
}  // namespace volumetric_kit::core
