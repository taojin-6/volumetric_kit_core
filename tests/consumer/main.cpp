// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Links volumetric_kit::core and uses each piece of the base tier once, a call
// of the net tier -- and the vulkan tier, when the core has it -- so a broken
// link or include path fails here rather than in a sibling. Nothing here needs
// a GPU.

#include <cstdio>
#include <string>
#include <string_view>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/net/address.hpp"
#include "volumetric_kit/core/version.hpp"

#ifdef VKC_CONSUMER_HAS_CONFIG
#include "volumetric_kit/core/config/config.hpp"
#endif

#ifdef VKC_CONSUMER_HAS_VULKAN
#include "smoke_comp.spv.hpp"
#include "volumetric_kit/core/vulkan/compute_pipeline.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#endif

namespace vkc = volumetric_kit::core;

// From the consumer's own library (parse.cpp).
vkc::Result<int> parse_positive(int x);

namespace {

vkc::Status run() {
  // The version vkc_require_core read (CMakeLists.txt) is the headers' and the
  // linked library's.
  if (std::string_view(VKC_VERSION_STRING) != VKC_CONSUMER_CORE_VERSION ||
      std::string_view(vkc::version_string()) != VKC_CONSUMER_CORE_VERSION) {
    return vkc::Status::invalid_argument("the core's versions disagree");
  }
#ifdef VKC_CONSUMER_HAS_CONFIG
  VKC_ASSIGN(const auto scenario,
             vkc::config::parse(R"({"version":1,"operation":"view"})",
                                vkc::config::Application::GfxTriangle));
  if (scenario.window.width != 800)
    return vkc::Status::invalid_argument("config defaults disagree");
  VKC_ASSIGN(
      const auto readback,
      vkc::config::parse(
          R"({"version":1,"operation":"stream","source":{"kind":"live","color_codec":"mjpeg"},"diagnostics":{"readback":true,"dump_sets":2,"dump_directory":"/dump"}})",
          vkc::config::Application::FuseOrbbec));
  if (!readback.diagnostics.readback)
    return vkc::Status::invalid_argument("config readback was not enabled");
  if (readback.diagnostics.dump_sets != 2 ||
      readback.diagnostics.dump_directory != "/dump")
    return vkc::Status::invalid_argument("config dump was not enabled");
  VKC_ASSIGN(
      const auto mesh,
      vkc::config::parse(
          R"({"version":1,"operation":"codec","source":{"kind":"mesh","path":"f%05d.obj","metres_per_unit":2,"up_axis":"y","allow_holes":true},"fusion":{"voxel_size_m":0.005,"truncation_m":0.04},"run":{"first":1,"frames":150}})",
          vkc::config::Application::CodecMesh));
  if (!mesh.source.up_vector.has_value())
    return vkc::Status::invalid_argument("mesh direction was not resolved");
  if (mesh.source.metres_per_unit != 2 || mesh.source.height_m != 0 ||
      !mesh.source.allow_holes || mesh.fusion.truncation_m != 0.04F ||
      mesh.run.first != 1 || mesh.run.frames != 150)
    return vkc::Status::invalid_argument("mesh sequence was not read");
  VKC_ASSIGN(
      const auto codec,
      vkc::config::parse(
          R"({"version":1,"operation":"codec","source":{"kind":"dataset","path":"room0"},"codec":{"intra_period":30,"update_size":128},"output":{"stream":"runs/frames.vrst"}})",
          vkc::config::Application::CodecReplica, "/scenario"));
  if (codec.output.stream != "/scenario/runs/frames.vrst" ||
      !codec.output.csv.empty())
    return vkc::Status::invalid_argument("codec outputs were not resolved");
  if (codec.codec.intra_period != 30 || codec.codec.update_size != 128)
    return vkc::Status::invalid_argument(
        "codec P-frame settings were not read");
  VKC_ASSIGN(
      const auto sets,
      vkc::config::parse(
          R"({"version":1,"operation":"fuse","source":{"kind":"live","sync_config":"rig.json","audio":"default"},"fusion":{"free_after":10},"codec":{"intra_period":30},"output":{"stream":"rig.vrst","serve":47801,"max_viewers":16}})",
          vkc::config::Application::FuseOrbbec, "/scenario"));
  if (!sets.fusion.dynamic || sets.fusion.free_after != 10 ||
      sets.source.audio != "default" || sets.codec.intra_period != 30 ||
      sets.output.stream != "/scenario/rig.vrst" ||
      sets.output.serve != 47801 || sets.output.max_viewers != 16 ||
      sets.output.path != "/scenario/fuse_orbbec.ply")
    return vkc::Status::invalid_argument("fuse_orbbec coding was not read");
  VKC_ASSIGN(
      const auto phone,
      vkc::config::parse(
          R"({"version":1,"operation":"fuse","source":{"kind":"live","sync_config":"rig.json","calibration":"world.json","mobile":"tcp://10.8.0.1:7101","mobile_registration":"phone/registration.json"},"diagnostics":{"track_log":"runs/lane"}})",
          vkc::config::Application::FuseOrbbec, "/scenario"));
  if (!phone.source.mobile || !phone.source.mobile->path.empty() ||
      phone.source.mobile->host != "10.8.0.1" ||
      phone.source.mobile->port != 7101 ||
      phone.source.mobile->registration !=
          "/scenario/phone/registration.json" ||
      phone.diagnostics.track_log != "/scenario/runs/lane")
    return vkc::Status::invalid_argument("fuse_orbbec's phone was not read");
  VKC_ASSIGN(
      const auto realsense,
      vkc::config::parse(
          R"({"version":1,"operation":"fuse","source":{"kind":"live","driver":"realsense","serial":"123"},"run":{"frames":900}})",
          vkc::config::Application::FuseRealsense, "/scenario"));
  if (!realsense.fusion.dynamic || realsense.source.serial != "123" ||
      realsense.source.color_width != 1280 ||
      realsense.source.depth_height != 720 || realsense.source.fps != 30 ||
      realsense.run.frames != 900 ||
      realsense.output.path != "/scenario/fuse_realsense.ply")
    return vkc::Status::invalid_argument(
        "fuse_realsense settings were not read");
  VKC_ASSIGN(
      const auto stereo,
      vkc::config::parse(
          R"({"version":1,"operation":"fuse","source":{"kind":"live","depth":"ffs","ffs_model":"ffs.onnx","ffs_plan_cache":"plans"}})",
          vkc::config::Application::FuseRealsense, "/scenario"));
  if (stereo.source.depth != vkc::config::DepthSource::Ffs ||
      stereo.source.emitter || stereo.source.depth_width != 960 ||
      stereo.source.depth_height != 544 ||
      stereo.source.ffs_model != "/scenario/ffs.onnx" ||
      stereo.source.ffs_plan_cache != "/scenario/plans")
    return vkc::Status::invalid_argument(
        "fuse_realsense's FFS depth was not read");
  VKC_ASSIGN(
      const auto served,
      vkc::config::parse(
          R"({"version":1,"operation":"fuse","source":{"kind":"live","driver":"realsense"},"codec":{"intra_period":30},"output":{"stream":"camera.vrst","serve":47811,"max_viewers":2}})",
          vkc::config::Application::FuseRealsense, "/scenario"));
  if (served.codec.intra_period != 30 ||
      served.output.stream != "/scenario/camera.vrst" ||
      served.output.serve != 47811 || served.output.max_viewers != 2 ||
      served.output.path != "/scenario/fuse_realsense.ply")
    return vkc::Status::invalid_argument("fuse_realsense coding was not read");
  VKC_ASSIGN(
      const auto pair,
      vkc::config::parse(
          R"({"version":1,"operation":"fuse","source":{"kind":"live","driver":"decklink","cameras":[{"id":"bm-1","input":"SDI 2"},{"id":"bm-2","input":"SDI 1"}],"pairs":[{"left":"bm-1","right":"bm-2"}],"calibration":"rig.json","ffs_model":"ffs.onnx","ffs_plan_cache":"plans"},"output":{"serve":47811}})",
          vkc::config::Application::FuseDecklink, "/scenario"));
  if (pair.source.cameras.size() != 2 ||
      pair.source.cameras[0].input != "SDI 2" ||
      pair.source.pairs.size() != 1 || pair.source.pairs[0].right != "bm-2" ||
      pair.source.calibration != "/scenario/rig.json" ||
      pair.source.depth_width != 960 || pair.source.fps != 30 ||
      pair.source.max_pair_skew_us != 16666 || pair.output.serve != 47811 ||
      pair.output.path != "/scenario/fuse_decklink.ply")
    return vkc::Status::invalid_argument(
        "fuse_decklink settings were not read");
  VKC_ASSIGN(
      const auto rig,
      vkc::config::parse(
          R"({"version":1,"operation":"calibrate","source":{"kind":"live","sync_config":"rig.json"},"output":{"path":"sessions"}})",
          vkc::config::Application::CalibRig, "/scenario"));
  if (rig.operation != vkc::config::Operation::Calibrate ||
      rig.source.color_codec != vkc::config::ColorCodec::Mjpeg ||
      rig.source.color_width != 3840 || rig.output.path != "/scenario/sessions")
    return vkc::Status::invalid_argument("calib_rig settings were not read");
  VKC_ASSIGN(
      const auto lenses,
      vkc::config::parse(
          R"({"version":1,"operation":"calibrate_intrinsics","source":{"kind":"live","driver":"decklink","cameras":[{"id":"bm-1","input":"SDI 2"},{"id":"bm-2","input":"SDI 1"}],"pairs":[{"left":"bm-1","right":"bm-2"}],"calibration":"lenses.json"},"output":{"path":"sessions"}})",
          vkc::config::Application::CalibRig, "/scenario"));
  if (lenses.operation != vkc::config::Operation::CalibrateIntrinsics ||
      lenses.source.driver != "decklink" || lenses.source.pairs.size() != 1 ||
      lenses.source.calibration != "/scenario/lenses.json" ||
      lenses.source.depth_width != 960 || lenses.source.fps != 30)
    return vkc::Status::invalid_argument(
        "calib_rig's DeckLink lens settings were not read");
  VKC_ASSIGN(
      const auto fit,
      vkc::config::parse(
          R"({"version":1,"operation":"calibrate","source":{"kind":"live","sync_config":"rig.json","calibration":"rig_calibration.json","color_exposure":200,"color_gain":32},"output":{"path":"results"}})",
          vkc::config::Application::CalibColor, "/scenario"));
  if (fit.source.color_codec != vkc::config::ColorCodec::Hevc ||
      fit.source.color_exposure != 200 || fit.source.color_gain != 32 ||
      fit.run.frames != 50 || fit.output.path != "/scenario/results")
    return vkc::Status::invalid_argument("calib_color settings were not read");
  VKC_ASSIGN(
      const auto live,
      vkc::config::parse(
          R"({"version":1,"operation":"view","source":{"kind":"stream","host":"lab","port":5000},"run":{"frames":3}})",
          vkc::config::Application::Player, "/scenario"));
  if (live.source.kind != vkc::config::SourceKind::Stream ||
      live.source.host != "lab" || live.source.port != 5000 ||
      !live.source.path.empty() || live.run.frames != 3)
    return vkc::Status::invalid_argument("player settings were not read");
#endif
  VKC_ASSIGN(const int n, parse_positive(3));
  VKC_CHECK(n == 3, "parsed what it was given");
  // The net tier, which every core builds: an address as typed, then one
  // host's.
  VKC_ASSIGN(const vkc::net::HostPort typed,
             vkc::net::parse_host_port("10.8.0.1:7101", 0));
  VKC_ASSIGN(const vkc::net::Endpoint lab,
             vkc::net::parse_endpoint(typed.host, typed.port));
  if (vkc::net::to_string(lab) != "10.8.0.1:7101") {
    return vkc::Status::invalid_argument("a net endpoint did not round-trip");
  }
  if (parse_positive(-1).ok()) return vkc::Status::numerical("accepted -1");
  if (vkc::read_file("").status().domain() !=
      vkc::Status::Code::InvalidArgument) {
    return vkc::Status::invalid_argument("read_file accepted an empty path");
  }
  try {
    throw 1;
  } catch (...) {
    if (vkc::exception_status("consumer", vkc::Status::Code::IoError)
            .domain() != vkc::Status::Code::IoError) {
      return vkc::Status::invalid_argument("exception_status lost the domain");
    }
  }
#ifdef VKC_CONSUMER_HAS_VULKAN
  if (vkc::to_string(VK_ERROR_DEVICE_LOST) != "VK_ERROR_DEVICE_LOST") {
    return vkc::Status::invalid_argument("VkResult names do not resolve");
  }
  vkc::DeviceRequirements graphics;
  graphics.queue_flags = VK_QUEUE_GRAPHICS_BIT;
  VKC_ASSIGN(const vkc::DeviceRequirements both,
             vkc::merge(graphics, vkc::DeviceRequirements{}));
  VKC_CHECK(both.queue_flags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT),
            "merged queue flags");
  VKC_CHECK(vkc::ComputePipelineDesc{}.cache == VK_NULL_HANDLE,
            "a pipeline builds through no cache unless given one");
  // Embedded SPIR-V: whole words, opening with the SPIR-V magic number.
  VKC_CHECK(vkc_consumer_smoke_comp_spv_size % 4 == 0 &&
                vkc_consumer_smoke_comp_spv_size > 20 &&
                vkc_consumer_smoke_comp_spv[0] == 0x03 &&
                vkc_consumer_smoke_comp_spv[3] == 0x07,
            "embedded SPIR-V");
  std::printf("vulkan tier consumed\n");
#endif
  return {};
}

}  // namespace

int main() {
  int logged = 0;
  vkc::set_log_handler(
      [&](vkc::LogLevel, std::string_view, std::string_view) { ++logged; });
  vkc::log_message(vkc::LogLevel::Info, "consumer", "consumed");
  const vkc::Status status = run();
  if (!status || logged != 1) {
    std::fprintf(stderr, "consumer failed: %s\n", status.message().c_str());
    return 1;
  }
  std::printf("volumetric_kit_core %s consumed\n", vkc::version_string());
  return 0;
}
