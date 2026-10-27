// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Links volumetric_kit::core and uses each piece of the base tier once -- and
// of the vulkan tier, when the core has it -- so a broken link or include path
// fails here rather than in a sibling. Nothing here needs a GPU.

#include <cstdio>
#include <string>
#include <string_view>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
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
          R"({"version":1,"operation":"fuse","source":{"kind":"live"},"fusion":{"free_after":10},"codec":{"intra_period":30},"output":{"stream":"rig.vrst","serve":47801}})",
          vkc::config::Application::FuseOrbbec, "/scenario"));
  if (!sets.fusion.dynamic || sets.fusion.free_after != 10 ||
      sets.codec.intra_period != 30 ||
      sets.output.stream != "/scenario/rig.vrst" ||
      sets.output.serve != 47801 ||
      sets.output.path != "/scenario/fuse_orbbec.ply")
    return vkc::Status::invalid_argument("fuse_orbbec coding was not read");
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
      const auto fit,
      vkc::config::parse(
          R"({"version":1,"operation":"calibrate","source":{"kind":"live","sync_config":"rig.json","calibration":"rig_calibration.json","color_exposure":200,"color_gain":32},"output":{"path":"results"}})",
          vkc::config::Application::CalibColor, "/scenario"));
  if (fit.source.color_codec != vkc::config::ColorCodec::Hevc ||
      fit.source.color_exposure != 200 || fit.source.color_gain != 32 ||
      fit.run.frames != 50 || fit.output.path != "/scenario/results")
    return vkc::Status::invalid_argument("calib_color settings were not read");
  VKC_ASSIGN(
      const auto viewer,
      vkc::config::parse(
          R"({"version":1,"operation":"view","source":{"kind":"live","sync_config":"rig.json"},"run":{"sets":3000},"output":{"screenshot":"frames/rig.bmp"}})",
          vkc::config::Application::RigViewer, "/scenario"));
  if (viewer.output.screenshot != "/scenario/frames/rig.bmp" ||
      viewer.run.sets != 3000)
    return vkc::Status::invalid_argument("rig_viewer frames were not read");
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
