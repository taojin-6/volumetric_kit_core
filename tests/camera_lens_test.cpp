// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/camera/lens.hpp"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/camera/geometry.hpp"

namespace volumetric_kit::core {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

TEST(Lens, ChecksRefuseInvalidValues) {
  EXPECT_TRUE(check_image_size({1280, 720}).ok());
  EXPECT_EQ(check_image_size({0, 720}).domain(), Status::Code::InvalidArgument);
  EXPECT_EQ(check_image_size({1280, 0}).domain(),
            Status::Code::InvalidArgument);

  EXPECT_TRUE(check_intrinsics({746.7, 746.5, 637.7, 346.4}).ok());
  EXPECT_TRUE(check_intrinsics({746.7, 746.5, -3.0, 0.0}).ok());
  for (const PinholeIntrinsics& k :
       {PinholeIntrinsics{0.0, 746.5, 637.7, 346.4},
        PinholeIntrinsics{746.7, -1.0, 637.7, 346.4},
        PinholeIntrinsics{std::nan(""), 746.5, 637.7, 346.4},
        PinholeIntrinsics{746.7, 746.5, kInf, 346.4},
        PinholeIntrinsics{746.7, 746.5, 637.7, std::nan("")}}) {
    EXPECT_EQ(check_intrinsics(k).domain(), Status::Code::InvalidArgument);
  }

  EXPECT_TRUE(check_distortion({}).ok());
  RationalDistortion d;
  d.k6 = kInf;
  EXPECT_EQ(check_distortion(d).domain(), Status::Code::InvalidArgument);
}

TEST(Lens, ScalingKeepsPixelCentresAligned) {
  // A principal point at the centre of a 3840x2160 image stays at the centre
  // of a 1920x1080 one: (W - 1) / 2 in both.
  const PinholeIntrinsics k{2239.5, 2239.0, 1919.5, 1079.5};
  const auto half = scale_intrinsics(k, {3840, 2160}, {1920, 1080});
  ASSERT_TRUE(half.ok());
  EXPECT_DOUBLE_EQ(half.value().fx, 1119.75);
  EXPECT_DOUBLE_EQ(half.value().fy, 1119.5);
  EXPECT_DOUBLE_EQ(half.value().cx, 959.5);
  EXPECT_DOUBLE_EQ(half.value().cy, 539.5);

  // Each axis scales by its own ratio, and scaling back restores the start.
  const PinholeIntrinsics off{1000.0, 1000.0, 600.25, 300.75};
  const auto there = scale_intrinsics(off, {1280, 720}, {640, 480});
  ASSERT_TRUE(there.ok());
  EXPECT_DOUBLE_EQ(there.value().fx, 500.0);
  EXPECT_DOUBLE_EQ(there.value().fy, 1000.0 * 480.0 / 720.0);
  const auto back = scale_intrinsics(there.value(), {640, 480}, {1280, 720});
  ASSERT_TRUE(back.ok());
  EXPECT_NEAR(back.value().cx, off.cx, 1e-12);
  EXPECT_NEAR(back.value().cy, off.cy, 1e-12);

  EXPECT_EQ(scale_intrinsics(k, {0, 2160}, {1920, 1080}).status().domain(),
            Status::Code::InvalidArgument);
  EXPECT_EQ(scale_intrinsics(k, {3840, 2160}, {1920, 0}).status().domain(),
            Status::Code::InvalidArgument);
}

TEST(Lens, DistortionTermsAreOpenCvs) {
  // No distortion moves nothing.
  const Vec2d p = distort_normalized({}, {0.3, -0.2});
  EXPECT_EQ(p.x, 0.3);
  EXPECT_EQ(p.y, -0.2);

  // Each term alone, by hand at (0.5, 0), where r^2 = 0.25.
  RationalDistortion k1;
  k1.k1 = 0.1;
  EXPECT_DOUBLE_EQ(distort_normalized(k1, {0.5, 0.0}).x, 0.5 * 1.025);
  RationalDistortion k4 = k1;
  k4.k4 = 0.1;  // the denominator cancels the numerator
  EXPECT_DOUBLE_EQ(distort_normalized(k4, {0.5, 0.0}).x, 0.5);
  RationalDistortion p1;
  p1.p1 = 0.01;  // y moves by p1 (r^2 + 2 y^2)
  EXPECT_DOUBLE_EQ(distort_normalized(p1, {0.5, 0.0}).y, 0.0025);
  RationalDistortion p2;
  p2.p2 = 0.01;  // x moves by p2 (r^2 + 2 x^2)
  EXPECT_DOUBLE_EQ(distort_normalized(p2, {0.5, 0.0}).x, 0.5 + 0.0075);
}

TEST(Lens, DistortionMatchesOpenCvsProjectPoints) {
  // cv::projectPoints with an identity camera matrix and this eight-
  // coefficient lens (OpenCV 5.0.0), at points out to r ~ 0.9.
  const RationalDistortion d{0.42,  -0.31, 0.0013, -0.0021,
                             0.087, 0.39,  -0.12,  0.051};
  struct Case {
    Vec2d in, out;
  };
  const Case cases[] = {
      {{0.0, 0.0}, {0.0, 0.0}},
      {{0.3, -0.2}, {0.29941270929159258, -0.19962147286106177}},
      {{-0.55, 0.4}, {-0.54180871681926501, 0.39393758950491992}},
      {{0.7, 0.55}, {0.65411586826839641, 0.51628605721088305}},
  };
  for (const Case& c : cases) {
    const Vec2d got = distort_normalized(d, c.in);
    EXPECT_NEAR(got.x, c.out.x, 1e-12);
    EXPECT_NEAR(got.y, c.out.y, 1e-12);
  }
}

}  // namespace
}  // namespace volumetric_kit::core
