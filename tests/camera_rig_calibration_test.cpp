// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/camera/rig_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/camera/geometry.hpp"
#include "volumetric_kit/core/camera/lens.hpp"

namespace volumetric_kit::core {
namespace {

constexpr double kPi = 3.14159265358979323846;

// The family's config layout, trimmed: an unrelated section, a non-object
// entry, a camera with every field, and one turned a quarter about +y with
// tvec (0, 0, 2) and nothing else.
const char* const kConfig = R"({
  "kinect_config": {"device_count": 2, "master_serial": "A"},
  "device_calibration": {
    "pose_calib_file_path": "/somewhere/all_extrinsics.npy",
    "A": {
      "intrinsics": {"fx": 1822.1, "fy": 1820.1, "cx": 1930.0, "cy": 1107.7,
                     "width": 3840, "height": 2160},
      "distortion": {"k1": -0.175, "k2": -2.855, "p1": 0.0023, "p2": 0.0006,
                     "k3": 2.184, "k4": -0.282, "k5": -2.634, "k6": 2.063},
      "optimal_intrinsics": {"fx": 1892.1, "fy": 1861.0, "cx": 1933.5,
                             "cy": 1111.1, "width": 3840, "height": 2160},
      "pose": {"rvec": [0.0, 0.0, 0.0], "tvec": [0.0, 0.0, 0.0]}
    },
    "B": {"pose": {"rvec": [0.0, 1.5707963267948966, 0.0],
                   "tvec": [0.0, 0.0, 2.0]}}
  },
  "SenderToUnity": {"host": "0.0.0.0", "port": 33669}
})";

// One camera with a pose and whatever `fields` adds, as a document.
std::string camera_doc(const std::string& fields) {
  return R"({"device_calibration": {"A": {"pose": {"rvec": [0, 0, 0],
      "tvec": [0, 0, 0]})" +
         (fields.empty() ? "" : ", " + fields) + "}}}";
}

Status::Code parse_domain(const std::string& json) {
  const auto r = parse_rig_calibration(json);
  return r.ok() ? Status::Code::Ok : r.status().domain();
}

void expect_near(const Vec3d& a, const Vec3d& b, double eps) {
  EXPECT_NEAR(a.x, b.x, eps);
  EXPECT_NEAR(a.y, b.y, eps);
  EXPECT_NEAR(a.z, b.z, eps);
}

RigidTransform pose(Vec3d axis, double angle, Vec3d t) {
  const double n =
      std::sqrt((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z));
  return {rotation_from_rodrigues(
              {axis.x / n * angle, axis.y / n * angle, axis.z / n * angle}),
          t};
}

std::string scratch(const char* name) { return ::testing::TempDir() + name; }

// The value of an optional the test has already asserted is present.
template <typename T>
T got(const std::optional<T>& o) {
  return o.value_or(T{});
}

bool write_text(const std::string& path, const std::string& text) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool written =
      std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && written;
}

bool exists(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  (void)std::fclose(f);
  return true;
}

TEST(RigCalibration, ParsesTheFamilysLayout) {
  const auto r = parse_rig_calibration(kConfig);
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r.value().size(), 2U);
  const RigCameraCalibration& a = r.value()[0];
  const RigCameraCalibration& b = r.value()[1];
  EXPECT_EQ(a.serial, "A");
  EXPECT_EQ(b.serial, "B");

  ASSERT_TRUE(a.intrinsics && a.optimal_intrinsics && a.distortion &&
              a.image_size);
  EXPECT_EQ(got(a.intrinsics).fx, 1822.1);
  EXPECT_EQ(got(a.intrinsics).cy, 1107.7);
  EXPECT_EQ(got(a.optimal_intrinsics).cx, 1933.5);
  EXPECT_EQ(got(a.distortion).k2, -2.855);
  EXPECT_EQ(got(a.distortion).k6, 2.063);
  EXPECT_EQ(got(a.image_size).width, 3840U);
  EXPECT_EQ(got(a.image_size).height, 2160U);
  expect_near(a.camera_to_world * Vec3d{1.0, 2.0, 3.0}, {1.0, 2.0, 3.0}, 0.0);
  EXPECT_FALSE(b.intrinsics || b.distortion || b.optimal_intrinsics ||
               b.image_size);

  // B's pose is world -> camera, x_cam = R x_world + t, so its centre is
  // -R^T t, and the point 1 m ahead of it, (0, 0, 1) in its frame, is
  // R^T((0, 0, 1) - t). R takes +x to -z, so R^T (0, 0, -1) = (1, 0, 0).
  expect_near(b.camera_to_world.translation, {2.0, 0.0, 0.0}, 1e-15);
  expect_near(b.camera_to_world * Vec3d{0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, 1e-15);
}

TEST(RigCalibration, ReadsTheDistortionModel) {
  const std::string coefficients =
      R"("k1": 0, "k2": 0, "p1": 0, "p2": 0, "k3": 0, "k4": 0, "k5": 0, "k6": 0)";
  EXPECT_EQ(parse_domain(camera_doc(R"("distortion": {)" + coefficients + "}")),
            Status::Code::Ok);
  EXPECT_EQ(parse_domain(camera_doc(R"("distortion": {"model": "rational", )" +
                                    coefficients + "}")),
            Status::Code::Ok);
  EXPECT_EQ(
      parse_domain(camera_doc(R"("distortion": {"model": "kannala_brandt", )" +
                              coefficients + "}")),
      Status::Code::Unsupported);
  EXPECT_EQ(parse_domain(camera_doc(R"("distortion": {"model": 1, )" +
                                    coefficients + "}")),
            Status::Code::InvalidArgument);
  // Five coefficients are not a rational lens.
  EXPECT_EQ(
      parse_domain(camera_doc(
          R"("distortion": {"k1": 0, "k2": 0, "p1": 0, "p2": 0, "k3": 0})")),
      Status::Code::InvalidArgument);
}

TEST(RigCalibration, ReadsTheImageSize) {
  const std::string k = R"("fx": 1, "fy": 1, "cx": 0, "cy": 0)";
  // No size is fine; so is one block's alone.
  EXPECT_EQ(parse_domain(camera_doc(R"("intrinsics": {)" + k + "}")),
            Status::Code::Ok);
  const auto one = parse_rig_calibration(
      camera_doc(R"("intrinsics": {)" + k + R"(}, "optimal_intrinsics": {)" +
                 k + R"(, "width": 640, "height": 480})"));
  ASSERT_TRUE(one.ok()) << one.status().message();
  ASSERT_TRUE(one.value()[0].image_size);
  EXPECT_EQ(got(one.value()[0].image_size).width, 640U);

  for (const std::string& size :
       {std::string(R"("width": 640)"),               // no height
        std::string(R"("width": 0, "height": 480)"),  // a zero side
        std::string(R"("width": -640, "height": 480)"),
        std::string(R"("width": 640.5, "height": 480)"),
        std::string(R"("width": 4294967296, "height": 480)"),
        std::string(R"("width": "640", "height": 480)")}) {
    std::string block = R"("intrinsics": {)";
    block += k;
    block += ", ";
    block += size;
    block += "}";
    EXPECT_EQ(parse_domain(camera_doc(block)), Status::Code::InvalidArgument)
        << size;
  }
  // The two blocks must agree.
  EXPECT_EQ(parse_domain(camera_doc(R"("intrinsics": {)" + k +
                                    R"(, "width": 640, "height": 480},
                "optimal_intrinsics": {)" +
                                    k + R"(, "width": 1280, "height": 720})")),
            Status::Code::InvalidArgument);
}

TEST(RigCalibration, RefusesMalformedDocuments) {
  const char* const refused[] = {
      "{not json",
      R"({"kinect_config": {}})",
      R"([1, 2, 3])",
      R"({"device_calibration": {"pose_calib_file_path": "x"}})",
      R"({"device_calibration": {"A": {"intrinsics":
          {"fx": 1, "fy": 1, "cx": 0, "cy": 0}}}})",  // no pose
      R"({"device_calibration": {"A": {"pose":
          {"rvec": [0, 0], "tvec": [0, 0, 0]}}}})",
      R"({"device_calibration": {"A": {"pose":
          {"rvec": [0, 0, 0], "tvec": [0, "0", 0]}}}})",
      R"({"device_calibration": {"A": {"pose":
          {"rvec": [0, 0, 0], "tvec": [0, 0, 1e999]}}}})",
      R"({"device_calibration": {"A": {"pose":
          {"rvec": [0, 0, 0], "tvec": [0, 0, 0]},
          "intrinsics": {"fx": 0, "fy": 1, "cx": 0, "cy": 0}}}})",
      R"({"device_calibration": {"A": {"pose":
          {"rvec": [0, 0, 0], "tvec": [0, 0, 0]},
          "intrinsics": {"fx": 1, "fy": 1, "cx": 0}}}})",
  };
  for (const char* json : refused) {
    const auto r = parse_rig_calibration(json);
    ASSERT_FALSE(r.ok()) << json;
    EXPECT_EQ(r.status().domain(), Status::Code::InvalidArgument) << json;
  }
}

TEST(RigCalibration, NamesWhereADocumentIsNotJson) {
  // Without exceptions too: the parser's error comes through its SAX seam.
  const auto r = parse_rig_calibration("{\"device_calibration\": {,}}");
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("not JSON"), std::string::npos);
  EXPECT_NE(r.status().message().find("column"), std::string::npos)
      << r.status().message();
}

TEST(RigCalibration, RoundTripsThroughAFile) {
  // Rodrigues' edge cases -- no rotation, a tiny one, a general one, turns of
  // pi and just under it -- and a camera with every field.
  const std::vector<RigCameraCalibration> written = {
      {"id", {}, {}, {}, {}, {}},
      {"tiny", pose({0.3, 0.5, 0.8}, 1e-7, {0.1, 0.2, 0.3}), {}, {}, {}, {}},
      {"general", pose({0.3, -0.8, 0.52}, 0.7, {1.25, -2.5, 3.75e-3}),
       ImageSize{1280, 720},
       PinholeIntrinsics{746.494, 746.319, 630.403, 345.615},
       RationalDistortion{0.07536, -0.10528, -0.000272, 0.000335, 0.04364, 0.0,
                          0.0, 0.0},
       PinholeIntrinsics{740.1, 741.2, 631.0, 346.0}},
      {"pi_x", pose({1, 0, 0}, kPi, {0, 0, 1}), {}, {}, {}, {}},
      {"pi_xy", pose({1, 1, 0}, kPi, {0.5, 0, 0}), {}, {}, {}, {}},
      {"near_pi", pose({0.2, 0.9, -0.4}, 3.1415, {0, 1, 0}), {}, {}, {}, {}},
      {"\xC3\xA9t\xC3\xA9", {}, {}, {}, {}, {}},  // "été": UTF-8 serials pass
  };
  const std::string path = scratch("rig_calibration.json");
  ASSERT_TRUE(write_rig_calibration(path, written).ok());
  const auto read = read_rig_calibration(path);
  ASSERT_TRUE(read.ok()) << read.status().message();
  ASSERT_EQ(read.value().size(), written.size());
  for (const RigCameraCalibration& w : written) {
    const auto it =
        std::find_if(read.value().begin(), read.value().end(),
                     [&](const auto& c) { return c.serial == w.serial; });
    ASSERT_NE(it, read.value().end()) << w.serial;
    // The pose passes through Rodrigues and back; every other number is
    // written in a form that reads back exactly.
    for (const Vec3d p : {Vec3d{0, 0, 0}, Vec3d{1, -2, 3}}) {
      expect_near(it->camera_to_world * p, w.camera_to_world * p, 1e-12);
    }
    EXPECT_EQ(it->image_size.has_value(), w.image_size.has_value());
    EXPECT_EQ(it->intrinsics.has_value(), w.intrinsics.has_value());
    if (!w.intrinsics) continue;
    EXPECT_EQ(got(it->image_size).width, got(w.image_size).width);
    EXPECT_EQ(got(it->image_size).height, got(w.image_size).height);
    EXPECT_EQ(got(it->intrinsics).fx, got(w.intrinsics).fx);
    EXPECT_EQ(got(it->intrinsics).cy, got(w.intrinsics).cy);
    EXPECT_EQ(got(it->optimal_intrinsics).fy, got(w.optimal_intrinsics).fy);
    EXPECT_EQ(got(it->distortion).p1, got(w.distortion).p1);
    EXPECT_EQ(got(it->distortion).k3, got(w.distortion).k3);
  }

  // The formatted text is the file's, and parses on its own.
  const auto text = format_rig_calibration(written);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.value().find("\"model\": \"rational\""), std::string::npos);
  EXPECT_NE(text.value().find("\"width\": 1280"), std::string::npos);
  EXPECT_TRUE(parse_rig_calibration(text.value()).ok());
}

TEST(RigCalibration, WriterRefusesWhatTheReaderWould) {
  const std::string path = scratch("rig_calibration_refused.json");
  (void)std::remove(path.c_str());
  const auto refused = [&](const std::vector<RigCameraCalibration>& cameras) {
    const Status s = write_rig_calibration(path, cameras);
    return s.domain() == Status::Code::InvalidArgument;
  };
  EXPECT_TRUE(refused({}));
  EXPECT_TRUE(refused({{"", {}, {}, {}, {}, {}}}));
  EXPECT_TRUE(refused({{"A", {}, {}, {}, {}, {}}, {"A", {}, {}, {}, {}, {}}}));
  EXPECT_TRUE(refused({{"\xC0\xAF", {}, {}, {}, {}, {}}}));      // overlong '/'
  EXPECT_TRUE(refused({{"\xED\xA0\x80", {}, {}, {}, {}, {}}}));  // surrogate
  EXPECT_TRUE(refused({{"\xE2\x82", {}, {}, {}, {}, {}}}));      // truncated
  RigidTransform scaled;
  scaled.rotation.m[0][0] = 2.0;
  EXPECT_TRUE(refused({{"A", scaled, {}, {}, {}, {}}}));
  // A size is recorded beside intrinsics, so it needs some.
  EXPECT_TRUE(refused({{"A", {}, ImageSize{640, 480}, {}, {}, {}}}));
  EXPECT_TRUE(refused(
      {{"A", {}, ImageSize{640, 0}, PinholeIntrinsics{1, 1, 0, 0}, {}, {}}}));
  EXPECT_TRUE(refused({{"A", {}, {}, PinholeIntrinsics{1, 0, 0, 0}, {}, {}}}));
  RationalDistortion nan;
  nan.k5 = std::nan("");
  EXPECT_TRUE(refused({{"A", {}, {}, {}, nan, {}}}));
  // A refusal touches no file.
  EXPECT_FALSE(exists(path));
}

TEST(RigCalibration, ReadReportsTheFileAndKeepsTheDomain) {
  const auto missing = read_rig_calibration(scratch("no_such_rig.json"));
  EXPECT_EQ(missing.status().domain(), Status::Code::IoError);
  // A directory opens on some platforms; reading it fails either way.
  EXPECT_EQ(read_rig_calibration(::testing::TempDir()).status().domain(),
            Status::Code::IoError);

  const std::string path = scratch("rig_calibration_fisheye.json");
  ASSERT_TRUE(write_text(
      path,
      camera_doc(R"("distortion": {"model": "kannala_brandt", "k1": 0, "k2": 0,
          "p1": 0, "p2": 0, "k3": 0, "k4": 0, "k5": 0, "k6": 0})")));
  const auto unsupported = read_rig_calibration(path);
  EXPECT_EQ(unsupported.status().domain(), Status::Code::Unsupported);
  EXPECT_EQ(unsupported.status().message().rfind(path, 0), 0U)
      << unsupported.status().message();
}

}  // namespace
}  // namespace volumetric_kit::core
