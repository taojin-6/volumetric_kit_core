// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/camera/rig_calibration.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/camera/geometry.hpp"
#include "volumetric_kit/core/camera/lens.hpp"

namespace volumetric_kit::core {

namespace {

using nlohmann::json;

constexpr std::string_view kRationalModel = "rational";

Status bad(const std::string& what) {
  return Status::invalid_argument("rig calibration: " + what);
}

// Well-formed UTF-8, as Unicode's Table 3-7 defines it: no overlong forms, no
// surrogates, nothing past U+10FFFF. JSON text is UTF-8, and nlohmann's dump()
// aborts on anything else when built without exceptions, so a serial is held
// to this before it is written.
bool is_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;  // the second byte's range
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
    } else if (lead == 0xE0) {
      length = 3;
      low = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
      length = 3;
    } else if (lead == 0xED) {
      length = 3;
      high = 0x9F;
    } else if (lead == 0xF0) {
      length = 4;
      low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
      length = 4;
    } else if (lead == 0xF4) {
      length = 4;
      high = 0x8F;
    } else {
      return false;
    }
    if (s.size() - i < length) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto b = static_cast<unsigned char>(s[i + k]);
      if (b < (k == 1 ? low : 0x80) || b > (k == 1 ? high : 0xBF)) return false;
    }
    i += length;
  }
  return true;
}

// The message of a document's first syntax error. nlohmann hands a SAX
// consumer the error rather than throwing it, so a build without exceptions
// still names where the document went wrong.
struct ParseErrorReader final : nlohmann::json_sax<json> {
  bool null() override { return true; }
  bool boolean(bool /*val*/) override { return true; }
  bool number_integer(number_integer_t /*val*/) override { return true; }
  bool number_unsigned(number_unsigned_t /*val*/) override { return true; }
  bool number_float(number_float_t /*val*/, const string_t& /*s*/) override {
    return true;
  }
  bool string(string_t& /*val*/) override { return true; }
  bool binary(binary_t& /*val*/) override { return true; }
  bool start_object(std::size_t /*elements*/) override { return true; }
  bool key(string_t& /*val*/) override { return true; }
  bool end_object() override { return true; }
  bool start_array(std::size_t /*elements*/) override { return true; }
  bool end_array() override { return true; }
  bool parse_error(std::size_t /*position*/, const std::string& /*last_token*/,
                   const nlohmann::detail::exception& ex) override {
    message = ex.what();
    return false;
  }

  std::string message;
};

Result<double> number(const json& object, const char* key,
                      const std::string& where) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) {
    return bad(where + "missing number \"" + key + "\"");
  }
  const double v = it->get<double>();
  if (!std::isfinite(v)) return bad(where + "\"" + key + "\" is not finite");
  return v;
}

Result<std::uint32_t> pixels(const json& object, const char* key,
                             const std::string& where) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_unsigned() ||
      it->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
    return bad(where + "\"" + key + "\" is not a whole number of pixels");
  }
  return static_cast<std::uint32_t>(it->get<std::uint64_t>());
}

Result<PinholeIntrinsics> pinhole(const json& block, const std::string& where) {
  if (!block.is_object()) return bad(where + "not an object");
  PinholeIntrinsics k;
  VKC_ASSIGN(k.fx, number(block, "fx", where));
  VKC_ASSIGN(k.fy, number(block, "fy", where));
  VKC_ASSIGN(k.cx, number(block, "cx", where));
  VKC_ASSIGN(k.cy, number(block, "cy", where));
  return k;
}

// The image size an intrinsics block records, if it records one.
Result<std::optional<ImageSize>> recorded_size(const json& block,
                                               const std::string& where) {
  const bool has_width = block.contains("width");
  if (has_width != block.contains("height")) {
    return bad(where + R"(has one of "width" and "height")");
  }
  if (!has_width) return std::optional<ImageSize>();
  ImageSize size;
  VKC_ASSIGN(size.width, pixels(block, "width", where));
  VKC_ASSIGN(size.height, pixels(block, "height", where));
  return std::optional<ImageSize>(size);
}

Result<RationalDistortion> rational(const json& block,
                                    const std::string& where) {
  if (!block.is_object()) return bad(where + "not an object");
  const auto model = block.find("model");
  if (model != block.end()) {
    if (!model->is_string()) return bad(where + "\"model\" is not a string");
    const auto& name = model->get_ref<const std::string&>();
    if (name != kRationalModel) {
      return Status::unsupported("rig calibration: " + where + "model \"" +
                                 name + "\" is not supported");
    }
  }
  RationalDistortion d;
  VKC_ASSIGN(d.k1, number(block, "k1", where));
  VKC_ASSIGN(d.k2, number(block, "k2", where));
  VKC_ASSIGN(d.p1, number(block, "p1", where));
  VKC_ASSIGN(d.p2, number(block, "p2", where));
  VKC_ASSIGN(d.k3, number(block, "k3", where));
  VKC_ASSIGN(d.k4, number(block, "k4", where));
  VKC_ASSIGN(d.k5, number(block, "k5", where));
  VKC_ASSIGN(d.k6, number(block, "k6", where));
  return d;
}

Result<Vec3d> vector3(const json& object, const char* key,
                      const std::string& where) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_array() || it->size() != 3) {
    return bad(where + "\"" + key + "\" is not 3 numbers");
  }
  double v[3];
  for (std::size_t i = 0; i < 3; ++i) {
    const json& e = (*it)[i];
    if (!e.is_number() || !std::isfinite(e.get<double>())) {
      return bad(where + "\"" + key + "\" is not 3 finite numbers");
    }
    v[i] = e.get<double>();
  }
  return Vec3d{v[0], v[1], v[2]};
}

Result<RigCameraCalibration> parse_camera(const std::string& serial,
                                          const json& entry) {
  RigCameraCalibration camera;
  camera.serial = serial;
  const std::string where = "camera " + serial + ": ";
  const auto pose = entry.find("pose");
  if (pose == entry.end() || !pose->is_object()) {
    return bad(where + "no \"pose\" object");
  }
  VKC_ASSIGN(const Vec3d rvec, vector3(*pose, "rvec", where + "pose "));
  VKC_ASSIGN(const Vec3d tvec, vector3(*pose, "tvec", where + "pose "));
  camera.camera_to_world =
      inverse(RigidTransform{rotation_from_rodrigues(rvec), tvec});
  for (const auto& [key, field] :
       {std::pair{"intrinsics", &camera.intrinsics},
        std::pair{"optimal_intrinsics", &camera.optimal_intrinsics}}) {
    const auto block = entry.find(key);
    if (block == entry.end()) continue;
    const std::string w = where + key + " ";
    VKC_ASSIGN(*field, pinhole(*block, w));
    VKC_ASSIGN(const std::optional<ImageSize> size, recorded_size(*block, w));
    if (!size) continue;
    if (camera.image_size && (camera.image_size->width != size->width ||
                              camera.image_size->height != size->height)) {
      return bad(where +
                 "intrinsics and optimal_intrinsics record different sizes");
    }
    camera.image_size = size;
  }
  const auto distortion = entry.find("distortion");
  if (distortion != entry.end()) {
    VKC_ASSIGN(camera.distortion, rational(*distortion, where + "distortion "));
  }
  return camera;
}

// A number as JSON text: the shortest form that reads back to the same value.
std::string text(double v) { return json(v).dump(); }
std::string text(std::uint32_t v) { return json(v).dump(); }

std::string join(const std::vector<std::string>& parts,
                 std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += separator;
    out += parts[i];
  }
  return out;
}

// "key": value
std::string member(std::string_view key, const std::string& value) {
  return json(std::string(key)).dump() + ": " + value;
}

std::string object(const std::vector<std::string>& members) {
  return "{" + join(members, ", ") + "}";
}

std::string pinhole_text(const PinholeIntrinsics& k,
                         const std::optional<ImageSize>& size) {
  std::vector<std::string> members = {
      member("fx", text(k.fx)), member("fy", text(k.fy)),
      member("cx", text(k.cx)), member("cy", text(k.cy))};
  if (size) {
    members.push_back(member("width", text(size->width)));
    members.push_back(member("height", text(size->height)));
  }
  return object(members);
}

std::string distortion_text(const RationalDistortion& d) {
  return object({member("model", json(std::string(kRationalModel)).dump()),
                 member("k1", text(d.k1)), member("k2", text(d.k2)),
                 member("p1", text(d.p1)), member("p2", text(d.p2)),
                 member("k3", text(d.k3)), member("k4", text(d.k4)),
                 member("k5", text(d.k5)), member("k6", text(d.k6))});
}

std::string vector3_text(const Vec3d& v) {
  return "[" + join({text(v.x), text(v.y), text(v.z)}, ", ") + "]";
}

}  // namespace

Status validate_rig_calibration(
    const std::vector<RigCameraCalibration>& cameras) {
  if (cameras.empty()) return bad("no cameras");
  for (std::size_t i = 0; i < cameras.size(); ++i) {
    const RigCameraCalibration& c = cameras[i];
    if (c.serial.empty()) {
      return bad("camera " + std::to_string(i) + ": no serial");
    }
    if (!is_utf8(c.serial)) {
      return bad("camera " + std::to_string(i) + ": serial is not UTF-8");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (cameras[j].serial == c.serial) {
        return bad("camera " + c.serial + " appears twice");
      }
    }
    // The checks' own messages name the field: "intrinsics need ...".
    const std::string camera = "rig calibration: camera " + c.serial;
    VKC_TRY(check_rigid(c.camera_to_world).with_context(camera + ": pose"));
    if (c.intrinsics) {
      VKC_TRY(check_intrinsics(*c.intrinsics).with_context(camera));
    }
    if (c.optimal_intrinsics) {
      VKC_TRY(check_intrinsics(*c.optimal_intrinsics)
                  .with_context(camera + ": optimal_intrinsics"));
    }
    if (c.distortion) {
      VKC_TRY(check_distortion(*c.distortion).with_context(camera));
    }
    if (c.image_size) {
      if (!c.intrinsics && !c.optimal_intrinsics) {
        return bad("camera " + c.serial +
                   ": an image size is recorded only beside intrinsics");
      }
      VKC_TRY(check_image_size(*c.image_size).with_context(camera));
    }
  }
  return {};
}

Result<std::vector<RigCameraCalibration>> parse_rig_calibration(
    std::string_view json_text) {
  const json doc = json::parse(json_text.begin(), json_text.end(), nullptr,
                               /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    ParseErrorReader reader;
    (void)json::sax_parse(json_text.begin(), json_text.end(), &reader);
    return bad("not JSON: " + reader.message);
  }
  const auto section =
      doc.is_object() ? doc.find("device_calibration") : doc.end();
  if (!doc.is_object() || section == doc.end() || !section->is_object()) {
    return bad("no \"device_calibration\" object");
  }
  std::vector<RigCameraCalibration> cameras;
  for (auto it = section->begin(); it != section->end(); ++it) {
    if (!it->is_object()) continue;  // e.g. a path to another file
    VKC_ASSIGN(RigCameraCalibration camera, parse_camera(it.key(), *it));
    cameras.push_back(std::move(camera));
  }
  VKC_TRY(validate_rig_calibration(cameras));
  return cameras;
}

Result<std::vector<RigCameraCalibration>> read_rig_calibration(
    const std::string& path) {
  // stdio rather than a stream: a stream reports a failed read -- of a
  // directory, say -- as the end of the file.
  std::FILE* in = std::fopen(path.c_str(), "rb");
  if (in == nullptr) {
    return Status::io_error("rig calibration: cannot open " + path);
  }
  std::string json_text;
  char buf[4096];
  // A short read is the end of the file or an error; ferror tells which.
  for (;;) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), in);
    json_text.append(buf, n);
    if (n < sizeof(buf)) break;
  }
  const bool failed = std::ferror(in) != 0;
  (void)std::fclose(in);
  if (failed) return Status::io_error("rig calibration: cannot read " + path);
  Result<std::vector<RigCameraCalibration>> cameras =
      parse_rig_calibration(json_text);
  if (!cameras) return cameras.status().with_context(path);
  return cameras;
}

Result<std::string> format_rig_calibration(
    const std::vector<RigCameraCalibration>& cameras) {
  VKC_TRY(validate_rig_calibration(cameras));
  // Laid out by hand so each block stays on one line.
  std::vector<std::string> entries;
  for (const RigCameraCalibration& c : cameras) {
    std::vector<std::string> fields;
    if (c.intrinsics) {
      fields.push_back(
          member("intrinsics", pinhole_text(*c.intrinsics, c.image_size)));
    }
    if (c.distortion) {
      fields.push_back(member("distortion", distortion_text(*c.distortion)));
    }
    if (c.optimal_intrinsics) {
      fields.push_back(
          member("optimal_intrinsics",
                 pinhole_text(*c.optimal_intrinsics, c.image_size)));
    }
    const RigidTransform world_to_camera = inverse(c.camera_to_world);
    fields.push_back(member(
        "pose",
        object({member("rvec", vector3_text(rodrigues_from_rotation(
                                   world_to_camera.rotation))),
                member("tvec", vector3_text(world_to_camera.translation))})));
    entries.push_back("    " + json(c.serial).dump() + ": {\n      " +
                      join(fields, ",\n      ") + "\n    }");
  }
  return "{\n  \"device_calibration\": {\n" + join(entries, ",\n") +
         "\n  }\n}\n";
}

Status write_rig_calibration(const std::string& path,
                             const std::vector<RigCameraCalibration>& cameras) {
  VKC_ASSIGN(const std::string json_text, format_rig_calibration(cameras));
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return Status::io_error("rig calibration: cannot create " + path);
  }
  const bool written = std::fwrite(json_text.data(), 1, json_text.size(),
                                   file) == json_text.size();
  if (std::fclose(file) != 0 || !written) {
    return Status::io_error("rig calibration: cannot write " + path);
  }
  return {};
}

}  // namespace volumetric_kit::core
