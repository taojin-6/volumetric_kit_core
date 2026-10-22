// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin
#include "volumetric_kit/core/config/config.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>  // IWYU pragma: keep
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"

namespace volumetric_kit::core::config {
namespace {
using Json = nlohmann::json;
using App = Application;
Config defaults_impl(App application);

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw std::invalid_argument(std::string(path) + ": " + std::string(reason));
}

bool is_one_of(App app, std::initializer_list<App> apps) {
  return std::find(apps.begin(), apps.end(), app) != apps.end();
}
bool is_gfx(App app) {
  return is_one_of(
      app, {App::GfxTriangle, App::GfxImGui, App::GfxModel, App::GfxImage});
}
bool is_dataset(App app) {
  return is_one_of(app, {App::FuseReplica, App::CodecReplica, App::FuseRender,
                         App::FuseViewer});
}
bool is_codec(App app) {
  return is_one_of(app, {App::CodecReplica, App::CodecMesh});
}
// The profiles that code frames: the codec applications, and FuseOrbbec, which
// can code each fused set.
bool takes_codec(App app) { return is_codec(app) || app == App::FuseOrbbec; }
// The Orbbec profiles that can fuse. Not every live profile: CalibRig and
// Scanner are live too.
bool is_live_fusion(App app) {
  return is_one_of(app, {App::FuseOrbbec, App::RigViewer});
}
// The Orbbec profiles: those that can fuse, and CalibRig, which opens a rig
// from its sync file alone.
bool is_orbbec(App app) { return is_live_fusion(app) || app == App::CalibRig; }
struct ParseState {
  std::vector<std::string> fields;
  std::string up_axis;
};
bool has(const ParseState& state, std::string_view field) {
  return std::find(state.fields.begin(), state.fields.end(), field) !=
         state.fields.end();
}

class Reader {
 public:
  Reader(const Json& value, std::string path, ParseState& state)
      : value_(value), path_(std::move(path)), state_(state) {
    if (!value_.is_object()) fail(path_, "expected an object");
  }
  const Json* take(const char* key) {
    used_.insert(key);
    const auto it = value_.find(key);
    if (it == value_.end()) return nullptr;
    state_.fields.push_back(field(key));
    return &*it;
  }
  std::string field(const char* key) const {
    return path_.empty() ? std::string(key) : path_ + "." + key;
  }
  void finish() const {
    for (auto it = value_.begin(); it != value_.end(); ++it) {
      if (used_.count(it.key()) == 0)
        fail(field(it.key().c_str()), "unknown field");
    }
  }
  template <class T>
  void number(const char* key, T& out) {
    const Json* value = take(key);
    if (value == nullptr) return;
    if constexpr (std::is_integral_v<T>) {
      if (!value->is_number_integer()) fail(field(key), "expected an integer");
      if (value->is_number_unsigned()) {
        const auto n = value->get<std::uint64_t>();
        if (n > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
          fail(field(key), "integer is out of range");
        out = static_cast<T>(n);
      } else {
        const auto n = value->get<std::int64_t>();
        if (n < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
            n > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
          fail(field(key), "integer is out of range");
        out = static_cast<T>(n);
      }
    } else {
      if (!value->is_number()) fail(field(key), "expected a number");
      const double n = value->get<double>();
      if (!std::isfinite(n) ||
          std::abs(n) > static_cast<double>(std::numeric_limits<T>::max()))
        fail(field(key), "number is not finite or is out of range");
      out = static_cast<T>(n);
    }
  }
  void boolean(const char* key, bool& out) {
    const Json* value = take(key);
    if (value == nullptr) return;
    if (!value->is_boolean()) fail(field(key), "expected a boolean");
    out = value->get<bool>();
  }
  void string(const char* key, std::string& out, bool allow_empty = false) {
    const Json* value = take(key);
    if (value == nullptr) return;
    if (!value->is_string()) fail(field(key), "expected a string");
    out = value->get<std::string>();
    if (out.find('\0') != std::string::npos) fail(field(key), "embedded NUL");
    if (out.empty() && !allow_empty) fail(field(key), "must not be empty");
  }
  template <class T>
  void choice(const char* key, T& out,
              std::initializer_list<std::pair<std::string_view, T>> choices) {
    std::string name;
    string(key, name);
    if (name.empty()) return;
    for (const auto& choice : choices) {
      if (name == choice.first) {
        out = choice.second;
        return;
      }
    }
    fail(field(key), "unknown choice '" + name + "'");
  }
  template <std::size_t N>
  void array(const char* key, std::array<double, N>& out) {
    const Json* value = take(key);
    if (value == nullptr) return;
    if (!value->is_array() || value->size() != N)
      fail(field(key),
           "expected an array of " + std::to_string(N) + " numbers");
    for (std::size_t i = 0; i < N; ++i) {
      if (!(*value)[i].is_number()) fail(field(key), "expected finite numbers");
      out[i] = (*value)[i].get<double>();
      if (!std::isfinite(out[i])) fail(field(key), "expected finite numbers");
    }
  }

 private:
  const Json& value_;
  std::string path_;
  ParseState& state_;
  std::set<std::string> used_;
};

void read_source(const Json& json, Config& c, ParseState& state) {
  Reader r(json, "source", state);
  if (!json.contains("kind")) fail("source.kind", "is required");
  r.choice("kind", c.source.kind,
           {{"none", SourceKind::None},
            {"dataset", SourceKind::Dataset},
            {"live", SourceKind::Live},
            {"mesh", SourceKind::Mesh},
            {"model", SourceKind::Model},
            {"stream", SourceKind::Stream}});
#define STR(member) r.string(#member, c.source.member)
#define NUM(member) r.number(#member, c.source.member)
#define BOOL(member) r.boolean(#member, c.source.member)
  STR(format);
  STR(driver);
  STR(path);
  STR(host);
  NUM(port);
  STR(camera_parameters);
  STR(serial);
  STR(sync_config);
  STR(calibration);
  r.string("up_axis", state.up_axis);
  BOOL(preload);
  BOOL(apply_sync);
  BOOL(host_clock);
  NUM(color_width);
  NUM(color_height);
  NUM(depth_width);
  NUM(depth_height);
  NUM(fps);
  NUM(height_m);
  NUM(metres_per_unit);
  NUM(shell_voxels);
  BOOL(allow_holes);
#undef STR
#undef NUM
#undef BOOL
  r.choice("color_codec", c.source.color_codec,
           {{"hevc", ColorCodec::Hevc}, {"mjpeg", ColorCodec::Mjpeg}});
  r.choice("sdf_mode", c.source.sdf_mode,
           {{"signed", SdfMode::Signed}, {"shell", SdfMode::Shell}});
  if (json.contains("up_vector")) {
    std::array<double, 3> up{};
    r.array("up_vector", up);
    c.source.up_vector = up;
  }
  r.finish();
}
void read_fusion(const Json& json, Config& c, ParseState& state) {
  Reader r(json, "fusion", state);
#define NUM(member) r.number(#member, c.fusion.member)
#define BOOL(member) r.boolean(#member, c.fusion.member)
  NUM(voxel_size_m);
  NUM(truncation_m);
  NUM(min_depth_m);
  NUM(max_depth_m);
  NUM(max_weight);
  NUM(stride);
  NUM(mesh_every);
  NUM(dirty_every);
  NUM(buckets);
  NUM(max_buckets);
  NUM(fuse_every);
  NUM(occlusion_m);
  NUM(remesh_every);
  NUM(hold_ms);
  NUM(free_after);
  BOOL(share_vertices);
  BOOL(device_extract);
  BOOL(dynamic);
  BOOL(depth_within_color);
#undef NUM
#undef BOOL
  r.finish();
}
void read_codec(const Json& json, Config& c, ParseState& state) {
  Reader r(json, "codec", state);
  r.number("coefficient_count", c.codec.coefficient_count);
  r.number("quantization_scale", c.codec.quantization_scale);
  r.number("segment_size", c.codec.segment_size);
  r.number("update_size", c.codec.update_size);
  r.number("intra_period", c.codec.intra_period);
  r.number("encode_every", c.codec.encode_every);
  r.boolean("sweep", c.codec.sweep);
  r.boolean("inspect_only", c.codec.inspect_only);
  r.choice("quantization_table", c.codec.quantization_table,
           {{"uniform", QuantizationTable::Uniform},
            {"band", QuantizationTable::Band},
            {"radial", QuantizationTable::Radial}});
  r.finish();
}
void read_window(const Json& json, Config& c, ParseState& state) {
  Reader r(json, "window", state);
  r.number("width", c.window.width);
  r.number("height", c.window.height);
  r.boolean("overlay", c.window.overlay);
  r.boolean("validation", c.window.validation);
  r.finish();
}
void read_render(const Json& json, Config& c, ParseState& state) {
  Reader r(json, "render", state);
  r.number("yaw_degrees", c.render.yaw_degrees);
  r.number("pitch_degrees", c.render.pitch_degrees);
  r.number("follow", c.render.follow);
  r.choice("shading", c.render.shading,
           {{"unlit", Shading::Unlit},
            {"lit", Shading::Lit},
            {"normals", Shading::Normals}});
  r.boolean("texture", c.render.texture);
  r.boolean("show_sources", c.render.show_sources);
  r.boolean("texture_stats", c.render.texture_stats);
  r.finish();
}
void read_calibration(const Json& json, Config& c, ParseState& state) {
  Reader r(json, "calibration", state);
#define NUM(member) r.number(#member, c.calibration.member)
  NUM(image_width);
  NUM(image_height);
#undef NUM
  r.array("intrinsics", c.calibration.intrinsics);
  r.array("distortion", c.calibration.distortion);
  r.array("point", c.calibration.point);
  r.finish();
}

void positive(double value, std::string_view name) {
  if (!std::isfinite(value) || value <= 0)
    fail(name, "must be finite and positive");
}
void nonnegative(double value, std::string_view name) {
  if (!std::isfinite(value) || value < 0)
    fail(name, "must be finite and nonnegative");
}
void path_check(const std::string& value, std::string_view name,
                bool required = false) {
  if (required && value.empty()) fail(name, "is required");
  if (value.find('\0') != std::string::npos) fail(name, "embedded NUL");
}

bool field_in(std::string_view field,
              std::initializer_list<std::string_view> list) {
  return std::find(list.begin(), list.end(), field) != list.end();
}

bool supports(std::string_view field, App app) {
  if (field_in(field, {"version", "operation", "source", "source.kind"}))
    return true;
  if (field == "source.path")
    return is_dataset(app) ||
           is_one_of(app, {App::CodecMesh, App::GfxModel, App::Player});
  if (field_in(field, {"source.host", "source.port"}))
    return app == App::Player;
  if (field_in(field,
               {"source.format", "source.camera_parameters", "source.preload"}))
    return is_dataset(app);
  if (field_in(field, {"source.driver"}))
    return is_orbbec(app) || app == App::Scanner;
  if (field_in(field,
               {"source.serial", "source.calibration", "source.apply_sync"}))
    return is_live_fusion(app);
  if (field_in(field,
               {"source.sync_config", "source.color_codec",
                "source.color_width", "source.color_height",
                "source.depth_width", "source.depth_height", "source.fps"}))
    return is_orbbec(app);
  if (field == "source.host_clock") return app == App::FuseOrbbec;
  if (field_in(field, {"source.height_m", "source.metres_per_unit",
                       "source.up_axis", "source.up_vector", "source.sdf_mode",
                       "source.shell_voxels", "source.allow_holes"}))
    return app == App::CodecMesh;
  if (field == "fusion" || field == "fusion.voxel_size_m")
    return !is_gfx(app) && !is_one_of(app, {App::CalibProjectPoint,
                                            App::CalibRig, App::Player});
  if (field == "fusion.truncation_m")
    return is_dataset(app) || is_one_of(app, {App::FuseOrbbec, App::RigViewer,
                                              App::Scanner, App::CodecMesh});
  if (field_in(field, {"fusion.min_depth_m", "fusion.max_depth_m",
                       "fusion.max_weight"}))
    return is_dataset(app) || is_one_of(app, {App::FuseOrbbec, App::RigViewer});
  if (field_in(field, {"fusion.stride", "fusion.mesh_every",
                       "fusion.dirty_every", "fusion.device_extract"}))
    return app == App::FuseReplica;
  if (field == "fusion.buckets")
    return app == App::FuseReplica || app == App::Scanner;
  if (field_in(field, {"fusion.max_buckets", "fusion.fuse_every"}))
    return app == App::Scanner;
  if (field == "fusion.share_vertices")
    return is_one_of(app, {App::FuseReplica, App::FuseRender, App::FuseViewer});
  if (field == "fusion.dynamic") return is_dataset(app) || is_live_fusion(app);
  if (field_in(field, {"fusion.depth_within_color", "fusion.hold_ms"}))
    return app == App::RigViewer;
  if (field == "fusion.free_after") return is_live_fusion(app);
  if (field == "fusion.occlusion_m")
    return app == App::RigViewer || app == App::Scanner;
  if (field == "fusion.remesh_every")
    return is_one_of(app, {App::FuseViewer, App::RigViewer, App::Scanner});
  if (field == "codec" || field.rfind("codec.", 0) == 0) {
    if (field == "codec.inspect_only") return app == App::CodecMesh;
    if (field == "codec.encode_every") return app == App::CodecReplica;
    return takes_codec(app);
  }
  if (field == "window" || field_in(field, {"window.width", "window.height"}))
    return is_gfx(app) ||
           is_one_of(app, {App::FuseRender, App::FuseViewer, App::RigViewer,
                           App::CalibRig, App::Player});
  if (field == "window.validation")
    return is_gfx(app) || is_one_of(app, {App::FuseViewer, App::RigViewer,
                                          App::CalibRig, App::Player});
  if (field == "window.overlay")
    return is_one_of(
        app, {App::GfxImGui, App::GfxModel, App::FuseViewer, App::RigViewer});
  if (field == "render")
    return is_one_of(
        app, {App::FuseRender, App::FuseViewer, App::RigViewer, App::Scanner});
  if (field_in(field,
               {"render.yaw_degrees", "render.pitch_degrees", "render.follow"}))
    return app == App::FuseRender;
  if (field == "render.shading")
    return app == App::FuseRender || app == App::FuseViewer ||
           app == App::RigViewer;
  if (field == "render.texture")
    return app == App::FuseRender || app == App::FuseViewer ||
           app == App::RigViewer || app == App::Scanner;
  if (field == "render.show_sources" || field == "render.texture_stats")
    return app == App::RigViewer;
  if (field == "output")
    return is_codec(app) ||
           is_one_of(app, {App::FuseReplica, App::FuseOrbbec, App::FuseRender,
                           App::GfxModel, App::CalibRig});
  if (field == "output.path")
    return is_one_of(app, {App::FuseReplica, App::FuseOrbbec, App::FuseRender,
                           App::CalibRig});
  if (field == "output.prefix") return is_codec(app);
  if (field_in(field, {"output.csv", "output.stream"})) return takes_codec(app);
  if (field == "output.serve") return app == App::FuseOrbbec;
  if (field == "output.screenshot") return app == App::GfxModel;
  if (field == "run" || field == "run.frames")
    return is_dataset(app) || is_live_fusion(app) || is_gfx(app) ||
           is_one_of(app, {App::CodecMesh, App::Player});
  if (field == "run.sets") return app == App::RigViewer;
  if (field == "run.first") return app == App::CodecMesh;
  if (field == "calibration") return app == App::CalibProjectPoint;
  if (field_in(field, {"calibration.image_width", "calibration.image_height",
                       "calibration.intrinsics", "calibration.distortion",
                       "calibration.point"}))
    return app == App::CalibProjectPoint;
  if (field == "diagnostics")
    return is_one_of(app, {App::Scanner, App::FuseOrbbec});
  if (field == "diagnostics.measure_stages") return app == App::Scanner;
  if (field_in(field, {"diagnostics.readback", "diagnostics.dump_sets",
                       "diagnostics.dump_directory"}))
    return app == App::FuseOrbbec;
  return false;
}

bool ignored_when_inspecting(std::string_view field) {
  return field == "fusion" || field.rfind("fusion.", 0) == 0 ||
         field == "source.sdf_mode" || field == "source.shell_voxels" ||
         field == "source.allow_holes" || field == "output.csv" ||
         field == "output.stream" ||
         (field.rfind("codec.", 0) == 0 && field != "codec.inspect_only");
}

std::array<double, 3> axis_vector(std::string_view axis) {
  std::size_t pos = 0;
  double sign = 1;
  if (!axis.empty() && (axis[0] == '+' || axis[0] == '-')) {
    sign = axis[0] == '-' ? -1. : 1.;
    pos = 1;
  }
  if (axis.size() != pos + 1 || axis[pos] < 'x' || axis[pos] > 'z')
    fail("source.up_axis", "expected x, y, z, -x, -y or -z");
  std::array<double, 3> up{};
  up[static_cast<std::size_t>(axis[pos] - 'x')] = sign;
  return up;
}

void validate_impl(const Config& c, App app, const ParseState& state) {
  if (c.version != 1) fail("version", "only version 1 is supported");
  for (const std::string& field : state.fields) {
    if (!supports(field, app)) fail(field, "is not used by this application");
    if (app == App::CodecMesh && c.codec.inspect_only &&
        ignored_when_inspecting(field))
      fail(field, "is not used when codec.inspect_only is enabled");
  }
  const auto expected = defaults_impl(app);
  if (c.operation != expected.operation &&
      (app != App::FuseOrbbec || c.operation != Operation::Stream))
    fail("operation", "is not supported by this application");
  if (c.source.kind != expected.source.kind)
    fail("source.kind", "is not supported by this application");
  if (c.operation == Operation::Stream) {
    for (const char* section : {"fusion", "codec", "output"})
      if (has(state, section)) fail(section, "is not used by operation stream");
  }
  // FuseOrbbec codes only when it writes the codec's rows or frames, or serves
  // the frames.
  if (app == App::FuseOrbbec && has(state, "codec") && c.output.csv.empty() &&
      c.output.stream.empty() && c.output.serve == 0)
    fail("codec", "requires output.stream, output.csv or output.serve");
  path_check(c.source.serial, "source.serial");
  path_check(c.source.path, "source.path",
             is_dataset(app) || app == App::CodecMesh);
  path_check(c.source.camera_parameters, "source.camera_parameters");
  path_check(c.source.sync_config, "source.sync_config",
             is_one_of(app, {App::RigViewer, App::CalibRig}));
  path_check(c.source.calibration, "source.calibration");
  path_check(c.output.path, "output.path",
             c.operation != Operation::Stream &&
                 is_one_of(app, {App::FuseReplica, App::FuseOrbbec,
                                 App::FuseRender, App::CalibRig}));
  path_check(c.output.prefix, "output.prefix");
  path_check(c.output.screenshot, "output.screenshot");
  path_check(c.output.csv, "output.csv");
  path_check(c.output.stream, "output.stream");
  // Compared after resolution, so two spellings of one path are refused too.
  // FuseOrbbec's PLY is a third file.
  if (!c.output.stream.empty() && c.output.stream == c.output.csv)
    fail("output.stream", "must differ from output.csv");
  if (!c.output.path.empty() && c.output.stream == c.output.path)
    fail("output.stream", "must differ from output.path");
  if (!c.output.path.empty() && c.output.csv == c.output.path)
    fail("output.csv", "must differ from output.path");
  // A run never overwrites the rig files it reads.
  for (const auto& [output, output_name] :
       {std::pair{&c.output.path, "output.path"},
        std::pair{&c.output.csv, "output.csv"},
        std::pair{&c.output.stream, "output.stream"}}) {
    for (const auto& [source, source_name] :
         {std::pair{&c.source.sync_config, "source.sync_config"},
          std::pair{&c.source.calibration, "source.calibration"}}) {
      if (!output->empty() && *output == *source)
        fail(output_name, std::string("must differ from ") + source_name);
    }
  }
  if (is_dataset(app) && c.source.format != "replica")
    fail("source.format", "only replica is supported");
  if (is_orbbec(app)) {
    if (c.source.driver != "orbbec")
      fail("source.driver", "only orbbec is supported");
    if (!c.source.serial.empty() && !c.source.sync_config.empty())
      fail("source.serial", "cannot be combined with source.sync_config");
    if (c.source.apply_sync && c.source.sync_config.empty())
      fail("source.apply_sync", "requires source.sync_config");
    if (c.source.host_clock && !c.source.sync_config.empty())
      fail("source.host_clock", "cannot be combined with source.sync_config");
    if (has(state, "source.color_width") != has(state, "source.color_height"))
      fail("source", "color_width and color_height must be provided together");
    if (has(state, "source.depth_width") != has(state, "source.depth_height"))
      fail("source", "depth_width and depth_height must be provided together");
    positive(c.source.color_width, "source.color_width");
    positive(c.source.color_height, "source.color_height");
    positive(c.source.depth_width, "source.depth_width");
    positive(c.source.depth_height, "source.depth_height");
    positive(c.source.fps, "source.fps");
    if (c.source.color_codec != ColorCodec::Hevc &&
        c.source.color_codec != ColorCodec::Mjpeg)
      fail("source.color_codec", "unknown choice");
    // An H.265 frame is not self-contained, and a capture keeps the camera's
    // own coded colour.
    if (app == App::CalibRig && c.source.color_codec != ColorCodec::Mjpeg)
      fail("source.color_codec",
           "must be mjpeg: a capture saves the camera's JPEG");
  }
  if (app == App::Scanner && c.source.driver != "arkit")
    fail("source.driver", "only arkit is supported");
  if (app == App::CodecMesh) {
    // A declared unit, or a height the consumer fits the mesh to.
    const bool height = has(state, "source.height_m");
    const bool unit = has(state, "source.metres_per_unit");
    if (height && unit)
      fail("source", "height_m and metres_per_unit are mutually exclusive");
    if (!height && !unit)
      fail("source", "height_m or metres_per_unit is required");
    if (height)
      positive(c.source.height_m, "source.height_m");
    else
      positive(c.source.metres_per_unit, "source.metres_per_unit");
    if (has(state, "source.up_axis") && has(state, "source.up_vector"))
      fail("source", "up_axis and up_vector are mutually exclusive");
    if (!c.source.up_vector)
      fail("source.up_vector", "up_axis or up_vector is required");
    const auto& up = *c.source.up_vector;
    for (const double v : up)
      if (!std::isfinite(v)) fail("source.up_vector", "must be finite");
    const double norm_squared =
        (up[0] * up[0]) + (up[1] * up[1]) + (up[2] * up[2]);
    if (!std::isfinite(norm_squared) || norm_squared <= 0)
      fail("source.up_vector", "squared length must be finite and positive");
    if (c.source.sdf_mode != SdfMode::Signed &&
        c.source.sdf_mode != SdfMode::Shell)
      fail("source.sdf_mode", "unknown choice");
    // Its bound against voxel and truncation is the mesh conversion's.
    positive(c.source.shell_voxels, "source.shell_voxels");
    if (has(state, "source.allow_holes") && c.source.sdf_mode == SdfMode::Shell)
      fail("source.allow_holes", "is not used by source.sdf_mode shell");
  }
  if (app == App::Player) {
    // A stream file, or a live sender's stream.
    const bool path = has(state, "source.path");
    const bool host = has(state, "source.host");
    if (path && host) fail("source", "path and host are mutually exclusive");
    if (!path && !host) fail("source", "path or host is required");
    // The port is a field of its own, and the client speaks IPv4 alone.
    if (c.source.host.find(':') != std::string::npos)
      fail("source.host", "must not contain ':'; the port is source.port");
    if (has(state, "source.port") && !host)
      fail("source.port", "requires source.host");
  }
  positive(c.fusion.voxel_size_m, "fusion.voxel_size_m");
  if (c.fusion.voxel_size_m > std::numeric_limits<float>::max() / 8.F)
    fail("fusion.voxel_size_m", "eight times the voxel size must be finite");
  positive(c.fusion.truncation_m, "fusion.truncation_m");
  const float block_size = 8.F * c.fusion.voxel_size_m;
  if (!std::isfinite(block_size) || block_size <= 0 ||
      c.fusion.truncation_m / block_size > 127.F)
    fail("fusion.truncation_m", "must be at most 127 blocks of eight voxels");
  positive(c.fusion.min_depth_m, "fusion.min_depth_m");
  positive(c.fusion.max_depth_m, "fusion.max_depth_m");
  if (c.fusion.min_depth_m >= c.fusion.max_depth_m)
    fail("fusion", "min_depth_m must be less than max_depth_m");
  positive(c.fusion.max_weight, "fusion.max_weight");
  positive(c.fusion.stride, "fusion.stride");
  nonnegative(c.fusion.mesh_every, "fusion.mesh_every");
  nonnegative(c.fusion.dirty_every, "fusion.dirty_every");
  if (c.fusion.buckets <= 0 ||
      c.fusion.buckets > std::numeric_limits<std::int32_t>::max() / (8 * 512))
    fail("fusion.buckets", "must be in [1, INT32_MAX/(8*512)]");
  if (app == App::Scanner &&
      (c.fusion.max_buckets < c.fusion.buckets ||
       c.fusion.max_buckets >
           std::numeric_limits<std::int32_t>::max() / (8 * 512)))
    fail("fusion.max_buckets",
         "must be at least buckets and at most INT32_MAX/(8*512)");
  positive(c.fusion.fuse_every, "fusion.fuse_every");
  positive(c.fusion.remesh_every, "fusion.remesh_every");
  nonnegative(c.fusion.hold_ms, "fusion.hold_ms");
  nonnegative(c.fusion.free_after, "fusion.free_after");
  if (app == App::Scanner)
    nonnegative(c.fusion.occlusion_m, "fusion.occlusion_m");
  else
    positive(c.fusion.occlusion_m, "fusion.occlusion_m");
  if (takes_codec(app)) {
    if (c.codec.coefficient_count < 1 || c.codec.coefficient_count > 512)
      fail("codec.coefficient_count", "must be in [1, 512]");
    if (!std::isnormal(c.codec.quantization_scale) ||
        c.codec.quantization_scale <= 0)
      fail("codec.quantization_scale", "must be positive and normal");
    positive(c.codec.segment_size, "codec.segment_size");
    positive(c.codec.intra_period, "codec.intra_period");
    // All-intra coding writes no P-frame. The size's range is the codec's.
    if (has(state, "codec.update_size") && c.codec.intra_period == 1)
      fail("codec.update_size", "is not used when codec.intra_period is 1");
    nonnegative(c.codec.encode_every, "codec.encode_every");
    if (c.codec.quantization_table != QuantizationTable::Uniform &&
        c.codec.quantization_table != QuantizationTable::Band &&
        c.codec.quantization_table != QuantizationTable::Radial)
      fail("codec.quantization_table", "unknown choice");
  }
  positive(c.window.width, "window.width");
  positive(c.window.height, "window.height");
  if (app == App::GfxModel &&
      (c.window.width > 16384 || c.window.height > 16384))
    fail("window", "model screenshot dimensions must not exceed 16384");
  if (!std::isfinite(c.render.yaw_degrees))
    fail("render.yaw_degrees", "must be finite");
  if (!std::isfinite(c.render.pitch_degrees))
    fail("render.pitch_degrees", "must be finite");
  if (c.render.shading != Shading::Unlit && c.render.shading != Shading::Lit &&
      c.render.shading != Shading::Normals)
    fail("render.shading", "unknown choice");
  if (c.render.shading == Shading::Normals &&
      (app == App::FuseViewer || app == App::FuseRender))
    fail("render.shading", "normals is only supported by rig_viewer");
  if ((is_dataset(app) || is_one_of(app, {App::FuseOrbbec, App::CodecMesh})) &&
      c.run.frames < 1)
    fail("run.frames", "must be positive");
  nonnegative(c.run.first, "run.first");
  // The last frame number, first + frames - 1, must be an int too.
  if (app == App::CodecMesh && std::int64_t{c.run.first} + c.run.frames - 1 >
                                   std::numeric_limits<std::int32_t>::max())
    fail("run.frames", "run.first + run.frames - 1 must not exceed INT32_MAX");
  if (app == App::RigViewer && c.run.frames < 0)
    fail("run.frames", "must be nonnegative");
  if (is_gfx(app) && c.run.frames < -1)
    fail("run.frames", "must be -1 or nonnegative");
  // The player plays until its window closes unless it is given a count.
  if (app == App::Player && has(state, "run.frames") && c.run.frames < 1)
    fail("run.frames", "must be positive");
  nonnegative(c.run.sets, "run.sets");
  nonnegative(c.diagnostics.dump_sets, "diagnostics.dump_sets");
  path_check(c.diagnostics.dump_directory, "diagnostics.dump_directory");
  if (c.diagnostics.dump_sets > 0) {
    if (c.diagnostics.dump_directory.empty())
      fail("diagnostics.dump_directory",
           "is required when diagnostics.dump_sets is positive");
    // An H.265 frame is not self-contained, so only MJPEG can be written out.
    if (c.source.color_codec != ColorCodec::Mjpeg)
      fail("diagnostics.dump_sets", "requires source.color_codec mjpeg");
    // FuseOrbbec's run.frames counts camera frames, at least one per set, so
    // a larger count of sets is never met.
    if (c.diagnostics.dump_sets > c.run.frames)
      fail("diagnostics.dump_sets", "must not exceed run.frames");
  } else if (!c.diagnostics.dump_directory.empty()) {
    fail("diagnostics.dump_directory",
         "is not used when diagnostics.dump_sets is 0");
  }
  if (app == App::GfxModel && !c.output.screenshot.empty()) {
    if (has(state, "run.frames"))
      fail("run.frames", "is not used when output.screenshot is requested");
    if (has(state, "window.overlay"))
      fail("window.overlay", "is not used when output.screenshot is requested");
  }
  if (app == App::CalibProjectPoint) {
    positive(c.calibration.image_width, "calibration.image_width");
    positive(c.calibration.image_height, "calibration.image_height");
    for (const double v : c.calibration.intrinsics)
      if (!std::isfinite(v)) fail("calibration.intrinsics", "must be finite");
    positive(c.calibration.intrinsics[0], "calibration.intrinsics[0]");
    positive(c.calibration.intrinsics[1], "calibration.intrinsics[1]");
    for (const double v : c.calibration.distortion)
      if (!std::isfinite(v)) fail("calibration.distortion", "must be finite");
    for (const double v : c.calibration.point)
      if (!std::isfinite(v)) fail("calibration.point", "must be finite");
    positive(c.calibration.point[2], "calibration.point[2]");
  }
}

// Parse configuration numbers consistently even when the caller uses directed
// rounding. nlohmann rejects overflowing numbers when strtod returns infinity.
class NearestRounding {
 public:
  NearestRounding() : original_(std::fegetround()) {
    if (original_ == -1 || std::fesetround(FE_TONEAREST) != 0)
      throw std::runtime_error("cannot select nearest numeric rounding");
  }
  ~NearestRounding() { std::fesetround(original_); }
  NearestRounding(const NearestRounding&) = delete;
  NearestRounding& operator=(const NearestRounding&) = delete;

 private:
  int original_;
};

void resolve_path(std::string& path, const std::filesystem::path& base) {
  if (path.empty()) return;
  std::filesystem::path p(path);
  if (p.is_relative()) p = base / p;
  path = p.lexically_normal().string();
}
// `base` with each '%' doubled, which a printf-style pattern reads as text.
std::filesystem::path escape_percent(const std::filesystem::path& base) {
  std::string escaped;
  for (const char ch : base.string()) {
    escaped += ch;
    if (ch == '%') escaped += '%';
  }
  return escaped;
}
void resolve(Config& c, App app, std::string_view directory,
             const ParseState& state) {
  if (app != App::Scanner &&
      (!has(state, "fusion.truncation_m") || c.fusion.truncation_m <= 0))
    c.fusion.truncation_m = 4.F * c.fusion.voxel_size_m;
  if (c.operation == Operation::Stream) c.output.path.clear();
  if (!state.up_axis.empty()) c.source.up_vector = axis_vector(state.up_axis);
  const std::filesystem::path base = directory.empty()
                                         ? std::filesystem::current_path()
                                         : std::filesystem::path(directory);
  // CodecMesh's path is a frame-number pattern; only what the user wrote is
  // pattern, so the directory joined onto it is escaped.
  resolve_path(c.source.path,
               app == App::CodecMesh ? escape_percent(base) : base);
  resolve_path(c.source.camera_parameters, base);
  resolve_path(c.source.sync_config, base);
  resolve_path(c.source.calibration, base);
  resolve_path(c.output.path, base);
  resolve_path(c.output.prefix, base);
  resolve_path(c.output.screenshot, base);
  resolve_path(c.output.csv, base);
  resolve_path(c.output.stream, base);
  resolve_path(c.diagnostics.dump_directory, base);
  if (is_dataset(app) && c.source.camera_parameters.empty() &&
      !c.source.path.empty())
    c.source.camera_parameters =
        (std::filesystem::path(c.source.path) / ".." / "cam_params.json")
            .lexically_normal()
            .string();
}

}  // namespace

namespace {
Config defaults_impl(Application application) {
  if (static_cast<int>(application) < 0 || application > App::Player)
    fail("application", "unknown application");
  Config c;
  if (is_dataset(application)) {
    c.source.kind = SourceKind::Dataset;
    c.source.format = "replica";
    c.operation = is_codec(application) ? Operation::Codec : Operation::Fuse;
    c.run.frames =
        application == App::FuseRender || application == App::FuseViewer
            ? 400
            : 1 << 30;
  }
  if (is_live_fusion(application)) {
    c.source.kind = SourceKind::Live;
    c.source.driver = "orbbec";
    c.fusion.min_depth_m = 0.25F;
    c.fusion.max_depth_m = 5.F;
    c.fusion.dynamic = true;
    c.operation =
        application == App::FuseOrbbec ? Operation::Fuse : Operation::View;
    c.run.frames = application == App::FuseOrbbec ? 300 : 0;
  }
  if (application == App::CodecMesh) {
    c.source.kind = SourceKind::Mesh;
    c.operation = Operation::Codec;
    c.run.frames = 1;
  }
  if (is_codec(application) || application == App::RigViewer ||
      application == App::Scanner)
    c.fusion.voxel_size_m = 0.01F;
  c.fusion.truncation_m = 4.F * c.fusion.voxel_size_m;
  if (application == App::FuseReplica) c.output.path = "fuse_room0.ply";
  if (application == App::FuseOrbbec) c.output.path = "fuse_orbbec.ply";
  if (application == App::FuseRender) c.output.path = "fuse_render.png";
  if (application == App::FuseViewer) c.render.shading = Shading::Lit;
  if (is_gfx(application)) {
    c.window.width = application == App::GfxTriangle ? 800 : 1024;
    c.window.height = application == App::GfxTriangle ? 600 : 720;
    c.window.validation = true;
    c.source.kind =
        application == App::GfxModel ? SourceKind::Model : SourceKind::None;
  }
  if (application == App::Scanner) {
    c.operation = Operation::Fuse;
    c.source.kind = SourceKind::Live;
    c.source.driver = "arkit";
    c.fusion.truncation_m = 0.03F;
    c.fusion.buckets = 1024;
    c.fusion.occlusion_m = 0.02F;
  }
  if (application == App::CalibProjectPoint) {
    c.source.kind = SourceKind::None;
    c.operation = Operation::Project;
  }
  if (application == App::CalibRig) {
    c.operation = Operation::Calibrate;
    c.source.kind = SourceKind::Live;
    c.source.driver = "orbbec";
    c.source.color_codec = ColorCodec::Mjpeg;
    c.source.color_width = 3840;
    c.source.color_height = 2160;
    c.source.depth_width = 640;
    c.source.depth_height = 576;
    c.source.fps = 25;
  }
  if (application == App::Player) {
    c.operation = Operation::View;
    c.source.kind = SourceKind::Stream;
  }
  return c;
}

}  // namespace

Result<Config> parse(std::string_view text, Application application,
                     std::string_view base_directory) {
  try {
    if (text.size() > kMaxConfigBytes) fail("config", "exceeds one MiB");
    const NearestRounding rounding;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&keys](int depth, Json::parse_event_t event,
                                  Json& value) {
      if ((event == Json::parse_event_t::object_start ||
           event == Json::parse_event_t::array_start) &&
          depth >= static_cast<int>(kMaxConfigDepth))
        fail("config", "nesting exceeds 32 levels");
      if (event == Json::parse_event_t::object_start) keys.emplace_back();
      if (event == Json::parse_event_t::key &&
          !keys.back().insert(value.get<std::string>()).second)
        fail("config", "duplicate key '" + value.get<std::string>() + "'");
      if (event == Json::parse_event_t::object_end) keys.pop_back();
      return true;
    };
    const Json json = Json::parse(text.begin(), text.end(), callback);
    Config c = defaults_impl(application);
    ParseState state;
    Reader r(json, "", state);
    if (!json.contains("version")) fail("version", "is required");
    r.number("version", c.version);
    if (!json.contains("operation")) fail("operation", "is required");
    r.choice("operation", c.operation,
             {{"fuse", Operation::Fuse},
              {"stream", Operation::Stream},
              {"codec", Operation::Codec},
              {"view", Operation::View},
              {"project", Operation::Project},
              {"calibrate", Operation::Calibrate}});
    if (!json.contains("source") &&
        (is_dataset(application) || is_orbbec(application) ||
         is_one_of(application, {App::CodecMesh, App::Scanner, App::Player})))
      fail("source", "is required");
    if (const auto* v = r.take("source")) read_source(*v, c, state);
    if (const auto* v = r.take("fusion")) read_fusion(*v, c, state);
    if (const auto* v = r.take("codec")) read_codec(*v, c, state);
    if (const auto* v = r.take("window")) read_window(*v, c, state);
    if (const auto* v = r.take("render")) read_render(*v, c, state);
    if (const auto* v = r.take("calibration")) read_calibration(*v, c, state);
    if (const auto* v = r.take("run")) {
      Reader q(*v, "run", state);
      q.number("frames", c.run.frames);
      q.number("sets", c.run.sets);
      q.number("first", c.run.first);
      q.finish();
    }
    if (const auto* v = r.take("output")) {
      Reader q(*v, "output", state);
      q.string("path", c.output.path, true);
      q.string("prefix", c.output.prefix, true);
      q.string("screenshot", c.output.screenshot, true);
      q.string("csv", c.output.csv, true);
      q.string("stream", c.output.stream, true);
      q.number("serve", c.output.serve);
      q.finish();
    }
    if (const auto* v = r.take("diagnostics")) {
      Reader q(*v, "diagnostics", state);
      q.boolean("measure_stages", c.diagnostics.measure_stages);
      q.boolean("readback", c.diagnostics.readback);
      q.number("dump_sets", c.diagnostics.dump_sets);
      q.string("dump_directory", c.diagnostics.dump_directory);
      q.finish();
    }
    r.finish();
    resolve(c, application, base_directory, state);
    validate_impl(c, application, state);
    return c;
  } catch (...) {
    return exception_status("config", Status::Code::InvalidArgument);
  }
}

Result<Config> load(const std::string& path, Application application) {
  try {
    VKC_TRY(check_path("config", path));
    const UniqueFd fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd.valid()) {
      if (errno == ENOENT || errno == ENOTDIR)
        return Status::not_found("config: " + path);
      return Status::io_error(
          "config: cannot open " + path + ": " +
          std::error_code(errno, std::generic_category()).message());
    }
    struct stat info{};
    if (fstat(fd.get(), &info) != 0)
      return Status::io_error("config: cannot stat " + path);
    if (!S_ISREG(info.st_mode))
      return Status::invalid_argument("config: expected a regular file: " +
                                      path);
    if (info.st_size < 0 ||
        static_cast<std::uintmax_t>(info.st_size) > kMaxConfigBytes)
      return Status::invalid_argument("config: exceeds one MiB: " + path);
    std::string text;
    std::array<char, 4096> chunk{};
    for (;;) {
      const auto count = read(fd.get(), chunk.data(), chunk.size());
      if (count < 0) {
        if (errno == EINTR) continue;
        return Status::io_error("config: cannot read " + path);
      }
      if (count == 0) break;
      if (text.size() + static_cast<std::size_t>(count) > kMaxConfigBytes)
        return Status::invalid_argument("config: exceeds one MiB: " + path);
      text.append(chunk.data(), static_cast<std::size_t>(count));
    }
    const auto base = std::filesystem::absolute(std::filesystem::path(path))
                          .parent_path()
                          .string();
    auto result = parse(text, application, base);
    if (!result) return result.status().with_context(path);
    return result;
  } catch (...) {
    return exception_status("load config", Status::Code::IoError);
  }
}

Result<Arguments> parse_args(int argc, const char* const* argv) {
  try {
    if (argc < 1 || argv == nullptr || argv[0] == nullptr)
      return Status::invalid_argument("config: invalid arguments");
    Arguments args;
    for (int i = 1; i < argc; ++i) {
      if (argv[i] == nullptr)
        return Status::invalid_argument("config: null argument");
      const std::string_view arg(argv[i]);
      if (arg == "--help" || arg == "-h") {
        args.help = true;
        continue;
      }
      if (arg != "--config")
        return Status::invalid_argument("unknown argument: " +
                                        std::string(arg));
      if (!args.config_path.empty())
        return Status::invalid_argument("--config was supplied twice");
      ++i;
      if (i >= argc || argv[i] == nullptr ||
          std::string_view(argv[i]).empty() ||
          std::string_view(argv[i]).rfind("--", 0) == 0)
        return Status::invalid_argument("--config requires a path");
      args.config_path = argv[i];
    }
    if (!args.help && args.config_path.empty())
      return Status::invalid_argument("--config is required");
    return args;
  } catch (...) {
    return exception_status("config arguments", Status::Code::InvalidArgument);
  }
}
}  // namespace volumetric_kit::core::config
