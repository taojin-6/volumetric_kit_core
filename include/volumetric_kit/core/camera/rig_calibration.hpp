// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file rig_calibration.hpp
/// @brief A fixed rig's calibration file: each camera's pose and lens, as
///        calib writes it and a capture reads it.
///
/// The file is the `device_calibration` object of a JSON document, one entry
/// per camera keyed by its serial. Other sections are ignored, and so are
/// entries in it that are not objects:
///
/// @code{.json}
/// {"device_calibration": {
///   "A": {
///     "intrinsics": {"fx": 746.7, "fy": 746.5, "cx": 637.7, "cy": 346.4,
///                    "width": 1280, "height": 720},
///     "distortion": {"model": "rational", "k1": 0.08, "k2": -0.1, "p1": 0,
///                    "p2": 0, "k3": 0.04, "k4": 0, "k5": 0, "k6": 0},
///     "optimal_intrinsics": {"fx": 740.2, "fy": 741.0, "cx": 636.9,
///                            "cy": 345.8, "width": 1280, "height": 720},
///     "pose": {"rvec": [0, 0, 0], "tvec": [0, 0, 0]}}}}
/// @endcode
///
/// - `pose`, the one required field, is the camera's OpenCV extrinsic:
///   `x_camera = R(rvec) x_world + tvec`, `rvec` a Rodrigues vector in
///   radians and `tvec` in metres. A rig's world is usually its reference
///   camera's frame.
/// - `intrinsics` are the camera's own; `optimal_intrinsics` are the pinhole
///   camera of its undistorted image. Both are in pixels of the image whose
///   `width` and `height` they record. The two are given together or not at
///   all, and must agree between the blocks.
/// - `distortion` takes all eight coefficients. Its `model` is `"rational"`,
///   the one this reader knows, and taken as that when absent.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/camera/export.hpp"
#include "volumetric_kit/core/camera/geometry.hpp"
#include "volumetric_kit/core/camera/lens.hpp"

namespace volumetric_kit::core {

/// @brief One camera of a calibrated rig.
///
/// @code
/// VKC_ASSIGN(std::vector<RigCameraCalibration> rig,
///            read_rig_calibration("rig_calibration.json"));
/// for (const RigCameraCalibration& camera : rig) {
///   const Vec3d centre = camera.camera_to_world.translation;
/// }
/// @endcode
struct RigCameraCalibration {
  std::string serial;  ///< How a capture finds the camera.
  /// The camera's frame to the world's: the inverse of the file's `pose`, in
  /// OpenCV's camera axes (x right, y down, z forward), in metres.
  RigidTransform camera_to_world;
  /// The image @ref intrinsics and @ref optimal_intrinsics are in pixels of,
  /// if the file records it.
  std::optional<ImageSize> image_size;
  std::optional<PinholeIntrinsics> intrinsics;          ///< If the file has it.
  std::optional<RationalDistortion> distortion;         ///< If the file has it.
  std::optional<PinholeIntrinsics> optimal_intrinsics;  ///< If the file has it.
};

/// @brief Parse a calibration document.
/// @param json  The document's text.
/// @return One entry per camera, ordered by serial; or
///         @ref Status::Code::InvalidArgument naming what is wrong (not JSON,
///         no `device_calibration` object or no camera in it, a camera without
///         a `pose` of two 3-vectors, a field of the wrong type, or a
///         calibration @ref validate_rig_calibration refuses); or
///         @ref Status::Code::Unsupported for a distortion model other than
///         `"rational"`.
VKC_CAMERA_API Result<std::vector<RigCameraCalibration>> parse_rig_calibration(
    std::string_view json);

/// @brief Read and parse the calibration file at @p path.
/// @param path  The file.
/// @return As @ref parse_rig_calibration, the message prefixed with @p path;
///         or @ref Status::Code::IoError if the file cannot be read.
VKC_CAMERA_API Result<std::vector<RigCameraCalibration>> read_rig_calibration(
    const std::string& path);

/// @brief The checks a calibration passes to be read or written.
///
/// At least one camera; each serial non-empty, UTF-8 and unique; each
/// `camera_to_world` rigid (@ref check_rigid); any intrinsics, distortion or
/// image size valid (@ref check_intrinsics, @ref check_distortion,
/// @ref check_image_size); and an image size only beside intrinsics, which is
/// where the file records it.
/// @param cameras  The calibration.
/// @return OK; or @ref Status::Code::InvalidArgument naming the camera and
///         what is wrong.
VKC_CAMERA_API Status
validate_rig_calibration(const std::vector<RigCameraCalibration>& cameras);

/// @brief Format @p cameras as a calibration document, the
///        `device_calibration` section alone.
///
/// Every number is written in the shortest form that reads back to the same
/// double, so @ref parse_rig_calibration returns the same intrinsics and
/// distortion, and the same pose to within the Rodrigues conversion's
/// round-off.
/// @param cameras  The calibration.
/// @return The document; or @ref Status::Code::InvalidArgument for a
///         calibration @ref validate_rig_calibration refuses.
VKC_CAMERA_API Result<std::string> format_rig_calibration(
    const std::vector<RigCameraCalibration>& cameras);

/// @brief Write @p cameras to @p path as @ref format_rig_calibration formats
///        them.
/// @param path     The file, created or replaced.
/// @param cameras  The calibration.
/// @return OK; @ref Status::Code::InvalidArgument for a calibration
///         @ref validate_rig_calibration refuses, before touching the file; or
///         @ref Status::Code::IoError if the file cannot be written.
VKC_CAMERA_API Status write_rig_calibration(
    const std::string& path, const std::vector<RigCameraCalibration>& cameras);

}  // namespace volumetric_kit::core
