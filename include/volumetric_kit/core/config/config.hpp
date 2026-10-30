// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin
#pragma once

/// @file config.hpp
/// @brief The family's versioned scenario settings; no JSON or GPU types.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/export.hpp"

namespace volumetric_kit::core::config {

/// @brief Application defaults and the capabilities a scenario may request.
enum class Application {
  FuseReplica,
  CodecReplica,
  CodecMesh,
  FuseOrbbec,
  FuseRender,
  FuseViewer,
  RigViewer,
  GfxTriangle,
  GfxImGui,
  GfxModel,
  GfxImage,
  Scanner,
  CalibProjectPoint,
  CalibRig,
  Player,
  CalibColor,
  FuseRealsense
};
/// @brief The work requested by a scenario.
enum class Operation { Fuse, Stream, Codec, View, Project, Calibrate };
/// @brief The source discriminator; branch-specific fields are checked
/// centrally.
enum class SourceKind { None, Dataset, Live, Mesh, Model, Stream };
/// @brief Camera colour encoding.
enum class ColorCodec { Hevc, Mjpeg };
/// @brief Mesh signed-distance interpretation.
enum class SdfMode { Signed, Shell };
/// @brief Quantization weight preset.
enum class QuantizationTable { Uniform, Band, Radial };
/// @brief Surface shading selection.
enum class Shading { Unlit, Lit, Normals };

/// @brief Input description. Paths returned by load/parse are config-relative.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Source {
  SourceKind kind = SourceKind::None;
  std::string format;
  std::string driver;
  /// Resolved against the config file. For CodecMesh it is a frame-number
  /// pattern for the consumer to read, `%%` being a literal `%`, so core
  /// escapes each `%` in the config file's directory.
  std::string path;
  std::string camera_parameters;
  std::string serial;
  std::string sync_config;
  std::string calibration;
  bool preload = false;
  bool apply_sync = false;
  bool host_clock = false;
  /// Every Orbbec profile's camera modes; CalibRig's colour is MJPEG only,
  /// CalibColor's H.265 only. FuseRealsense takes the sizes and rate, not
  /// the codec, and resolves colour 1280x800, depth 1280x720 at 30 fps.
  ColorCodec color_codec = ColorCodec::Hevc;
  std::uint32_t color_width = 3840;
  std::uint32_t color_height = 2160;
  std::uint32_t depth_width = 640;
  std::uint32_t depth_height = 576;
  std::uint32_t fps = 25;
  /// CalibColor: the manual exposure, in the camera's units, and the gain
  /// every colour camera is set to (auto exposure off, auto white balance
  /// on). Required and positive there; no other profile takes them.
  std::uint32_t color_exposure = 0;
  std::uint32_t color_gain = 0;
  /// CodecMesh scale: exactly one of `height_m` (a height the consumer scales
  /// the input to) and `metres_per_unit` (the input's declared unit) is
  /// positive; the other stays 0.
  double height_m = 0;
  double metres_per_unit = 0;
  std::optional<std::array<double, 3>> up_vector;
  SdfMode sdf_mode = SdfMode::Signed;
  /// Core checks only that it is finite and positive; its bound is the
  /// consumer's mesh conversion's.
  float shell_voxels = 1.5F;
  /// Signed mode admits a mesh whose only defect is boundary edges.
  bool allow_holes = false;
  /// Player: a live sender's host name or IPv4 address; empty plays `path`.
  std::string host;
  /// Player: the sender's port; 0, the default, is the sender's default
  /// (grid's networking::kDefaultPort).
  std::uint16_t port = 0;
};
/// @brief Fusion and mesh extraction settings.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Fusion {
  float voxel_size_m = 0.02F;
  float truncation_m = 0.08F;
  float min_depth_m = 0.1F;
  float max_depth_m = 8.F;
  float max_weight = 20.F;
  int stride = 1;
  int mesh_every = 50;
  int dirty_every = 0;
  int buckets = 16384;
  int max_buckets = 32768;
  int fuse_every = 1;
  bool share_vertices = false;
  bool device_extract = false;
  /// Dynamic integration if true, else Classic; FuseOrbbec, FuseRealsense
  /// and RigViewer resolve true.
  bool dynamic = false;
  float occlusion_m = 0.05F;
  int remesh_every = 1;
  int hold_ms = 2000;
  int free_after = 30;
};
/// @brief Codec settings; the codec maps the table to weights and checks steps.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Codec {
  std::uint32_t coefficient_count = 64;
  float quantization_scale = 0.2F;
  QuantizationTable quantization_table = QuantizationTable::Uniform;
  std::uint32_t segment_size = 64;
  /// The P-frame update-segment size, in entries; 0, the default, selects
  /// the codec's own size. The codec checks its range.
  std::uint32_t update_size = 0;
  /// An I-frame every `intra_period` frames, zero-motion P-frames between
  /// them; 1, the default, codes every frame intra.
  std::uint32_t intra_period = 1;
  int encode_every = 1;
  bool sweep = false;
  bool inspect_only = false;
};
/// @brief Presentation preferences independent of GLFW or any GPU API.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Window {
  int width = 1280;
  int height = 720;
  bool overlay = true;
  bool validation = false;
};
/// @brief View and render choices.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Render {
  float yaw_degrees = 45.F;
  float pitch_degrees = 30.F;
  int follow = -1;
  Shading shading = Shading::Unlit;
  bool texture = true;
  bool show_sources = false;
  bool texture_stats = false;
};
/// @brief Output paths, and FuseOrbbec's serving port and viewer limit. An
/// empty string disables each optional output; fusion/export applications,
/// CalibRig and CalibColor require a nonempty path.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Output {
  /// A fusion or export program's file; for CalibRig, the directory each
  /// session's own directory is created in; for CalibColor, the directory
  /// each run's own results directory is created in.
  std::string path;
  std::string prefix;
  std::string screenshot;
  /// The codec's per-frame rows, as CSV.
  std::string csv;
  /// The codec's coded frames, in one stream file. Once resolved, a nonempty
  /// `path`, `csv` and `stream` are three different files.
  std::string stream;
  /// FuseOrbbec's live serving port (grid's networking::StreamServer, on
  /// every interface); 0, the default, serves nothing.
  std::uint16_t serve = 0;
  /// The viewers `serve` serves at once, first come; positive, and given
  /// only with a nonzero `serve`.
  std::uint32_t max_viewers = 4;
};
/// @brief Run limits; a negative frame limit means unlimited where supported.
/// For CodecMesh, `frames` is instead an exact count of numbered meshes from
/// number `first`, where a dataset's is a limit that stops at its last frame.
/// FuseOrbbec's counts camera frames, each set adding one per camera it holds,
/// and FuseRealsense's its one camera's; CalibColor's counts sets.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Run {
  int frames = -1;
  int sets = 0;
  /// CodecMesh's first frame number.
  int first = 0;
};
/// @brief Settings for the small calibration projection demonstration.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Calibration {
  int image_width = 1280;
  int image_height = 720;
  std::array<double, 4> intrinsics{1000., 1000., 640., 360.};
  std::array<double, 5> distortion{-0.28, 0.10, 0.001, -0.0005, 0.};
  std::array<double, 3> point{0.1, -0.2, 2.};
};
/// @brief Optional timing and live-picture diagnostics.
/// @code /* Obtain resolved settings with load(path, application). */ @endcode
struct Diagnostics {
  bool measure_stages = true;
  /// Read each live set's luma to the host in FuseOrbbec's stream or fuse run.
  bool readback = false;
  /// FuseOrbbec writes the first dump_sets sets' camera JPEGs, and with
  /// readback their luma, into dump_directory; 0 writes none.
  int dump_sets = 0;
  /// The directory FuseOrbbec's dump writes into, config-relative; empty when
  /// off.
  std::string dump_directory;
};
/// @brief A validated scenario with all application defaults resolved.
/// @code
/// VKC_ASSIGN(const Config settings, load("live.json",
/// Application::FuseOrbbec));
/// @endcode
struct Config {
  int version = 1;
  Operation operation = Operation::View;
  Source source;
  Fusion fusion;
  Codec codec;
  Window window;
  Render render;
  Output output;
  Run run;
  Calibration calibration;
  Diagnostics diagnostics;
};
/// @brief Arguments accepted by every desktop entry point, without I/O.
/// @code VKC_ASSIGN(const Arguments args, parse_args(argc, argv)); @endcode
struct Arguments {
  bool help = false;
  std::string config_path;
};
/// Maximum bytes accepted by parse/load (one MiB).
inline constexpr std::size_t kMaxConfigBytes = std::size_t{1024} * 1024;
/// Maximum JSON object/array nesting accepted by parse/load.
inline constexpr std::size_t kMaxConfigDepth = 32;

/// @brief Parse a strict version-1 JSON scenario and apply application
/// defaults.
/// @param text UTF-8 JSON, bounded by kMaxConfigBytes and kMaxConfigDepth.
/// @param application The consumer selecting defaults and compatibility checks.
/// @param base_directory Directory against which relative paths are resolved.
/// @return Validated settings or a field-qualified failure.
VKC_CONFIG_API Result<Config> parse(std::string_view text,
                                    Application application,
                                    std::string_view base_directory = {});
/// @brief Read a bounded regular file and parse it relative to its directory.
/// @param path Configuration file path.
/// @param application The consumer selecting defaults and compatibility checks.
/// @return Validated settings; errors never escape as exceptions.
VKC_CONFIG_API Result<Config> load(const std::string& path,
                                   Application application);
/// @brief Parse only --config PATH and --help/-h; does not print or read files.
/// @param argc Argument count including the program name.
/// @param argv Arguments including the program name.
/// @return Help or a configuration path; all other arguments are errors.
VKC_CONFIG_API Result<Arguments> parse_args(int argc, const char* const* argv);

/// @brief Return the common command-line usage without allocation or printing.
/// @return A static, NUL-terminated usage string.
constexpr const char* usage() noexcept {
  return "Usage: <executable> --config PATH\n       <executable> --help\n";
}

}  // namespace volumetric_kit::core::config
