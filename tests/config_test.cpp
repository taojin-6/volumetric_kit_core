// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin
#include "volumetric_kit/core/config/config.hpp"

#include <cfenv>
#include <cstdint>
#include <cstdlib>  // IWYU pragma: keep; glibc declares mkdtemp here.
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>  // IWYU pragma: keep; macOS declares mkdtemp here.

#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core::config {
namespace {
using App = Application;

Result<Config> scenario(std::string_view text, App app,
                        std::string_view base = {}) {
  std::string document(text);
  if (!text.empty() && text.front() == '{') {
    const char* operation = "view";
    if (app == App::FuseReplica || app == App::FuseOrbbec ||
        app == App::FuseRealsense || app == App::FuseRender ||
        app == App::FuseViewer || app == App::Scanner)
      operation = "fuse";
    if (app == App::CodecReplica || app == App::CodecMesh) operation = "codec";
    if (app == App::CalibProjectPoint) operation = "project";
    if (app == App::CalibRig || app == App::CalibColor) operation = "calibrate";
    if (text.find("\"operation\"") == std::string_view::npos)
      document.insert(1, std::string(R"("operation":")") + operation + "\"" +
                             (text.size() > 2 ? "," : ""));
    const char* kind = "none";
    if (app == App::FuseReplica || app == App::CodecReplica ||
        app == App::FuseRender || app == App::FuseViewer)
      kind = "dataset";
    if (app == App::FuseOrbbec || app == App::FuseRealsense ||
        app == App::RigViewer || app == App::Scanner || app == App::CalibRig ||
        app == App::CalibColor)
      kind = "live";
    if (app == App::CodecMesh) kind = "mesh";
    if (app == App::GfxModel) kind = "model";
    if (app == App::Player) kind = "stream";
    const auto source = document.find("\"source\":{");
    if (source != std::string::npos &&
        document.find("\"kind\"") == std::string::npos)
      document.insert(source + 10, std::string(R"("kind":")") + kind + "\",");
    if (document.find("\"source\"") == std::string::npos &&
        (app == App::FuseOrbbec || app == App::FuseRealsense ||
         app == App::RigViewer || app == App::Scanner))
      document.insert(1, std::string(R"("source":{"kind":")") + kind + "\"},");
  }
  return parse(document, app, base);
}

TEST(Config, OperationIsRequiredAndUnknownApplicationIsRejected) {
  const auto missing = parse(R"({"version":1})", App::GfxModel);
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.status().message().find("operation"), std::string::npos);
  const auto invalid_app = static_cast<App>(999);
  EXPECT_FALSE(parse(R"({"version":1,"operation":"view"})", invalid_app));
}

void rejected(std::string_view text, App app, std::string_view field) {
  const auto result = scenario(text, app, "/scenario");
  ASSERT_FALSE(result) << text;
  EXPECT_EQ(result.status().domain(), Status::Code::InvalidArgument);
  EXPECT_NE(result.status().message().find(field), std::string::npos)
      << result.status().message();
}

TEST(Config, ApplicationDefaultsPreserveExistingBehavior) {
  const auto replica = scenario(R"({"version":1,"source":{"path":"scene"}})",
                                App::FuseReplica, "/scenario");
  ASSERT_TRUE(replica) << replica.status().message();
  EXPECT_EQ(replica->run.frames, 1 << 30);
  EXPECT_EQ(replica->fusion.voxel_size_m, 0.02F);
  EXPECT_EQ(replica->fusion.truncation_m, 0.08F);
  EXPECT_EQ(replica->fusion.mesh_every, 50);
  EXPECT_EQ(replica->source.path, "/scenario/scene");
  EXPECT_EQ(replica->source.camera_parameters, "/scenario/cam_params.json");
  EXPECT_EQ(replica->output.path, "/scenario/fuse_room0.ply");
  EXPECT_FALSE(replica->fusion.dynamic);
  EXPECT_FALSE(Config{}.fusion.dynamic);
  const auto codec = scenario(R"({"version":1,"source":{"path":"scene"}})",
                              App::CodecReplica, "/scenario");
  ASSERT_TRUE(codec) << codec.status().message();
  EXPECT_FALSE(codec->fusion.dynamic);
  EXPECT_TRUE(codec->output.prefix.empty());
  EXPECT_TRUE(codec->output.csv.empty());
  EXPECT_TRUE(codec->output.stream.empty());
  const auto mesh = scenario(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"}})",
      App::CodecMesh, "/scenario");
  ASSERT_TRUE(mesh) << mesh.status().message();
  // One mesh, number 0; a 4-voxel band; signed, holes refused.
  EXPECT_EQ(mesh->run.frames, 1);
  EXPECT_EQ(mesh->run.first, 0);
  EXPECT_EQ(mesh->fusion.voxel_size_m, 0.01F);
  EXPECT_EQ(mesh->fusion.truncation_m, 4.F * 0.01F);
  EXPECT_EQ(mesh->source.sdf_mode, SdfMode::Signed);
  EXPECT_FALSE(mesh->source.allow_holes);
  EXPECT_EQ(mesh->source.height_m, 2.);
  EXPECT_EQ(mesh->source.metres_per_unit, 0.);
  EXPECT_EQ(config::Run{}.first, 0);
  EXPECT_FALSE(config::Source{}.allow_holes);
  EXPECT_EQ(config::Source{}.metres_per_unit, 0.);
  const auto live = scenario(R"({"version":1})", App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(live) << live.status().message();
  // The live fusing profiles' camera modes: H.265 colour at 3840x2160, depth
  // at 640x576, 25 fps.
  const auto expect_4k25 = [](const config::Source& source) {
    EXPECT_EQ(source.color_codec, ColorCodec::Hevc);
    EXPECT_EQ(source.color_width, 3840U);
    EXPECT_EQ(source.color_height, 2160U);
    EXPECT_EQ(source.depth_width, 640U);
    EXPECT_EQ(source.depth_height, 576U);
    EXPECT_EQ(source.fps, 25U);
  };
  expect_4k25(live->source);
  expect_4k25(config::Source{});
  EXPECT_EQ(live->run.frames, 300);
  EXPECT_EQ(live->fusion.min_depth_m, 0.25F);
  EXPECT_EQ(live->fusion.max_depth_m, 5.F);
  EXPECT_TRUE(live->fusion.dynamic);
  const auto rig =
      scenario(R"({"version":1,"source":{"sync_config":"sync.json"}})",
               App::RigViewer, "/scenario");
  ASSERT_TRUE(rig) << rig.status().message();
  expect_4k25(rig->source);
  // The window is not the colour mode.
  EXPECT_EQ(rig->window.width, 1280);
  EXPECT_EQ(rig->window.height, 720);
  EXPECT_EQ(rig->fusion.voxel_size_m, 0.01F);
  EXPECT_EQ(rig->run.frames, 0);
  EXPECT_TRUE(rig->fusion.dynamic);
  EXPECT_EQ(rig->render.shading, Shading::Unlit);
  for (const auto app :
       {App::GfxTriangle, App::GfxImGui, App::GfxModel, App::GfxImage}) {
    const auto gfx = scenario(R"({"version":1})", app);
    ASSERT_TRUE(gfx) << gfx.status().message();
    EXPECT_EQ(gfx->window.width, app == App::GfxTriangle ? 800 : 1024);
    EXPECT_EQ(gfx->window.height, app == App::GfxTriangle ? 600 : 720);
    EXPECT_TRUE(gfx->window.validation);
    EXPECT_EQ(gfx->run.frames, -1);
  }
  const auto scanner = scenario(R"({"version":1})", App::Scanner);
  ASSERT_TRUE(scanner) << scanner.status().message();
  EXPECT_EQ(scanner->source.driver, "arkit");
  EXPECT_EQ(scanner->fusion.voxel_size_m, 0.01F);
  EXPECT_EQ(scanner->fusion.truncation_m, 0.03F);
  EXPECT_EQ(scanner->fusion.buckets, 1024);
  EXPECT_EQ(scanner->fusion.max_buckets, 32768);
  EXPECT_EQ(scanner->fusion.occlusion_m, 0.02F);
  EXPECT_TRUE(scanner->diagnostics.measure_stages);
}

TEST(Config, RequiredPathsAndSourceDiscriminatorsAreCheckedBeforeStartup) {
  rejected(R"({"version":1})", App::FuseReplica, "source");
  rejected(R"({"version":1})", App::RigViewer, "source.sync_config");
  rejected(R"({"version":1,"source":{"kind":"live","path":"scene"}})",
           App::FuseReplica, "source.kind");
  rejected(R"({"version":1,"source":{"driver":"other"}})", App::FuseOrbbec,
           "source.driver");
  rejected(R"({"version":1,"source":{"format":"other","path":"scene"}})",
           App::FuseReplica, "source.format");
}

TEST(Config, SchemaRefusesUnknownDuplicateAndWrongTypeFields) {
  rejected("{}", App::GfxImage, "version");
  rejected(R"({"version":2})", App::GfxImage, "version");
  rejected(R"({"version":1.0})", App::GfxImage, "version");
  rejected(R"({"version":true})", App::GfxImage, "version");
  rejected(R"({"version":1,"windwo":{}})", App::GfxImage, "windwo");
  rejected(R"({"version":1,"window":{"widht":100}})", App::GfxImage,
           "window.widht");
  rejected(R"({"version":1,"window":{"width":"100"}})", App::GfxImage,
           "window.width");
  rejected(R"({"version":1,"window":{"validation":1}})", App::GfxImage,
           "window.validation");
  rejected(R"({"version":1,"source":null})", App::GfxImage, "source");
  rejected(R"({"version":1,"version":1})", App::GfxImage, "duplicate key");
  rejected(R"({"version":1,"window":{"width":1,"width":2}})", App::GfxImage,
           "duplicate key");
  rejected(R"({"version":1,})", App::GfxImage, "parse");
  rejected("[]", App::GfxImage, "object");
}

TEST(Config, StreamModePreservesPresenceSensitiveFusionAndOutputConflicts) {
  const auto stream =
      scenario(R"({"version":1,"operation":"stream"})", App::FuseOrbbec);
  ASSERT_TRUE(stream) << stream.status().message();
  EXPECT_TRUE(stream->output.path.empty());
  rejected(R"({"version":1,"operation":"stream","fusion":{}})", App::FuseOrbbec,
           "fusion");
  rejected(
      R"({"version":1,"fusion":{"voxel_size_m":0.02},"operation":"stream"})",
      App::FuseOrbbec, "fusion");
  rejected(
      R"({"version":1,"operation":"stream","output":{"path":"fuse_orbbec.ply"}})",
      App::FuseOrbbec, "output");
  // Streaming codes nothing, so it refuses the codec section as it does the
  // outputs it would code to.
  rejected(R"({"version":1,"operation":"stream","codec":{}})", App::FuseOrbbec,
           "codec: is not used by operation stream");
  rejected(
      R"({"version":1,"operation":"stream","codec":{"intra_period":30},"output":{"stream":"rig.vrst"}})",
      App::FuseOrbbec, "codec: is not used by operation stream");
  rejected(
      R"({"version":1,"operation":"stream","output":{"stream":"rig.vrst"}})",
      App::FuseOrbbec, "output: is not used by operation stream");
  rejected(R"({"version":1,"operation":"stream"})", App::GfxModel, "operation");
}

TEST(Config, DynamicFusionIsAcceptedByEveryFusingProfile) {
  for (const auto app : {App::FuseReplica, App::CodecReplica, App::FuseRender,
                         App::FuseViewer, App::FuseOrbbec}) {
    const std::string source =
        app == App::FuseOrbbec ? "" : R"("source":{"path":"scene"},)";
    const auto parsed = scenario(
        R"({"version":1,)" + source + R"("fusion":{"dynamic":true}})", app);
    ASSERT_TRUE(parsed) << parsed.status().message();
    EXPECT_TRUE(parsed->fusion.dynamic);
  }
  const auto rig = scenario(
      R"({"version":1,"source":{"sync_config":"sync.json"},"fusion":{"dynamic":false}})",
      App::RigViewer);
  ASSERT_TRUE(rig) << rig.status().message();
  EXPECT_FALSE(rig->fusion.dynamic);
  const auto classic =
      scenario(R"({"version":1,"fusion":{"dynamic":false}})", App::FuseOrbbec);
  ASSERT_TRUE(classic) << classic.status().message();
  EXPECT_FALSE(classic->fusion.dynamic);
  rejected(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"},"fusion":{"dynamic":true}})",
      App::CodecMesh, "fusion.dynamic");
  rejected(R"({"version":1,"fusion":{"dynamic":true}})", App::Scanner,
           "fusion.dynamic");
  rejected(R"({"version":1,"fusion":{"dynamic":true}})", App::GfxModel,
           "fusion");
  rejected(R"({"version":1,"operation":"stream","fusion":{"dynamic":false}})",
           App::FuseOrbbec, "fusion: is not used by operation stream");
}

TEST(Config, CodecOutputsAreConfigRelativeAndOnlyWhereCoded) {
  // A dataset scenario with these output fields.
  const auto outputs = [](const std::string& fields) {
    return R"({"version":1,"source":{"path":"scene"},"output":{)" + fields +
           "}}";
  };
  const auto replica =
      scenario(outputs(R"("csv":"rows.csv","stream":"../runs/frames.vrst")"),
               App::CodecReplica, "/scenario/sub");
  ASSERT_TRUE(replica) << replica.status().message();
  EXPECT_EQ(replica->output.csv, "/scenario/sub/rows.csv");
  EXPECT_EQ(replica->output.stream, "/scenario/runs/frames.vrst");
  const auto mesh = scenario(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"},"output":{"csv":"/data/rows.csv","stream":"out/./mesh.vrst"}})",
      App::CodecMesh, "/scenario");
  ASSERT_TRUE(mesh) << mesh.status().message();
  EXPECT_EQ(mesh->output.csv, "/data/rows.csv");
  EXPECT_EQ(mesh->output.stream, "/scenario/out/mesh.vrst");
  const auto disabled = scenario(outputs(R"("csv":"","stream":"")"),
                                 App::CodecReplica, "/scenario");
  ASSERT_TRUE(disabled) << disabled.status().message();
  EXPECT_TRUE(disabled->output.csv.empty());
  EXPECT_TRUE(disabled->output.stream.empty());

  rejected(outputs(R"("csv":"a.csv")"), App::FuseReplica, "output.csv");
  rejected(R"({"version":1,"output":{"stream":"a.vrst"}})", App::GfxModel,
           "output.stream");
  rejected(outputs(R"("stream":"a.vrst")"), App::FuseRender,
           "output.stream: is not used by this application");
  for (const char* key : {"csv", "stream"}) {
    const std::string field = std::string("output.") + key;
    const std::string member = std::string("\"") + key + "\":";
    rejected(
        R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"},"codec":{"inspect_only":true},"output":{)" +
            member + R"("out"}})",
        App::CodecMesh,
        field + ": is not used when codec.inspect_only is enabled");
    rejected(outputs(member + "1"), App::CodecReplica,
             field + ": expected a string");
    rejected(outputs(member + R"("a\u0000b")"), App::CodecReplica,
             field + ": embedded NUL");
  }
  for (const char* paths :
       {R"("csv":"out.bin","stream":"out.bin")",
        R"("csv":"runs/../out.bin","stream":"./out.bin")",
        R"("csv":"/scenario/out.bin","stream":"out.bin")"}) {
    rejected(outputs(paths), App::CodecReplica,
             "output.stream: must differ from output.csv");
  }
}

TEST(Config, PictureReadbackIsOptionalForLiveFusionAndStreaming) {
  for (const char* operation : {"fuse", "stream"}) {
    const auto omitted = scenario(
        std::string(R"({"version":1,"operation":")") + operation + "\"}",
        App::FuseOrbbec);
    ASSERT_TRUE(omitted) << omitted.status().message();
    EXPECT_FALSE(omitted->diagnostics.readback);
    for (const char* enabled : {"true", "false"}) {
      const auto text = std::string(R"({"version":1,"operation":")") +
                        operation + R"(","diagnostics":{"readback":)" +
                        enabled + "}}";
      const auto loaded = scenario(text, App::FuseOrbbec);
      ASSERT_TRUE(loaded) << loaded.status().message();
      EXPECT_EQ(loaded->diagnostics.readback,
                std::string_view(enabled) == "true");
    }
  }
  rejected(R"({"version":1,"diagnostics":{"readback":1}})", App::FuseOrbbec,
           "diagnostics.readback");
  rejected(R"({"version":1,"diagnostics":{"readback":true}})", App::Scanner,
           "diagnostics.readback");
  rejected(R"({"version":1,"diagnostics":{"readback":false}})", App::Scanner,
           "diagnostics.readback");
  rejected(R"({"version":1,"diagnostics":{"measure_stages":false}})",
           App::FuseOrbbec, "diagnostics.measure_stages");
  rejected(
      R"({"version":1,"operation":"stream","diagnostics":{"readback":true},"fusion":{}})",
      App::FuseOrbbec, "fusion");
  rejected(
      R"({"version":1,"operation":"stream","diagnostics":{"readback":true},"output":{}})",
      App::FuseOrbbec, "output");
}

TEST(Config, JpegDumpIsAnOptionalConfigRelativeLiveDiagnostic) {
  const std::string mjpeg = R"({"version":1,"source":{"color_codec":"mjpeg"},)";
  const std::string no_directory =
      "diagnostics.dump_directory: is required when diagnostics.dump_sets is "
      "positive";
  const std::string unused_directory =
      "diagnostics.dump_directory: is not used when diagnostics.dump_sets is 0";
  const std::string not_mjpeg =
      "diagnostics.dump_sets: requires source.color_codec mjpeg";
  for (const char* operation : {"fuse", "stream"}) {
    const std::string run =
        std::string(R"({"version":1,"operation":")") + operation + "\",";
    const std::string head =
        run + R"("source":{"color_codec":"mjpeg"},"diagnostics":)";
    const auto omitted = scenario(head + "{}}", App::FuseOrbbec, "/scenario");
    ASSERT_TRUE(omitted) << omitted.status().message();
    EXPECT_EQ(omitted->diagnostics.dump_sets, 0);
    EXPECT_TRUE(omitted->diagnostics.dump_directory.empty());
    const auto dumped = scenario(
        head +
            R"({"readback":true,"dump_sets":20,"dump_directory":"h2/../dump"}})",
        App::FuseOrbbec, "/scenario");
    ASSERT_TRUE(dumped) << dumped.status().message();
    EXPECT_EQ(dumped->diagnostics.dump_sets, 20);
    EXPECT_EQ(dumped->diagnostics.dump_directory, "/scenario/dump");
    EXPECT_TRUE(dumped->diagnostics.readback);

    // A count needs a directory, and a directory a count.
    rejected(head + R"({"dump_sets":1}})", App::FuseOrbbec, no_directory);
    rejected(head + R"({"dump_directory":"dump"}})", App::FuseOrbbec,
             unused_directory);
    rejected(head + R"({"dump_sets":0,"dump_directory":"dump"}})",
             App::FuseOrbbec, unused_directory);
    // Only MJPEG frames are self-contained; HEVC is the default codec.
    rejected(run + R"("diagnostics":{"dump_sets":1,"dump_directory":"d"}})",
             App::FuseOrbbec, not_mjpeg);
    rejected(
        run +
            R"("source":{"color_codec":"hevc"},"diagnostics":{"dump_sets":1,"dump_directory":"d"}})",
        App::FuseOrbbec, not_mjpeg);
    // The run takes run.frames sets, so it can dump at most that many.
    const std::string frames =
        run + R"("source":{"color_codec":"mjpeg"},"run":{"frames":5},)";
    const auto all = scenario(
        frames + R"("diagnostics":{"dump_sets":5,"dump_directory":"d"}})",
        App::FuseOrbbec, "/scenario");
    ASSERT_TRUE(all) << all.status().message();
    EXPECT_EQ(all->diagnostics.dump_sets, 5);
    rejected(frames + R"("diagnostics":{"dump_sets":6,"dump_directory":"d"}})",
             App::FuseOrbbec,
             "diagnostics.dump_sets: must not exceed run.frames");
  }
  const auto absolute = scenario(
      mjpeg + R"("diagnostics":{"dump_sets":1,"dump_directory":"/data/h2"}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(absolute) << absolute.status().message();
  EXPECT_EQ(absolute->diagnostics.dump_directory, "/data/h2");
  const auto off = scenario(R"({"version":1,"diagnostics":{"dump_sets":0}})",
                            App::FuseOrbbec);
  ASSERT_TRUE(off) << off.status().message();
  EXPECT_EQ(off->diagnostics.dump_sets, 0);

  const auto count = [&](const char* value, const char* reason) {
    rejected(mjpeg + R"("diagnostics":{"dump_sets":)" + value +
                 R"(,"dump_directory":"d"}})",
             App::FuseOrbbec, std::string("diagnostics.dump_sets: ") + reason);
  };
  count("-1", "must be finite and nonnegative");
  count("1.5", "expected an integer");
  count("true", "expected an integer");
  count("2147483648", "integer is out of range");
  for (const char* sets : {"0", "1"}) {
    const auto directory = [&](const char* value, const char* reason) {
      rejected(mjpeg + R"("diagnostics":{"dump_sets":)" + sets +
                   R"(,"dump_directory":)" + value + "}}",
               App::FuseOrbbec,
               std::string("diagnostics.dump_directory: ") + reason);
    };
    directory(R"("")", "must not be empty");
    directory(R"("a\u0000b")", "embedded NUL");
    directory("1", "expected a string");
  }
  // FuseOrbbec only, even when the value would turn the dump off.
  rejected(R"({"version":1,"diagnostics":{"dump_sets":0}})", App::Scanner,
           "diagnostics.dump_sets: is not used by this application");
  rejected(R"({"version":1,"diagnostics":{"dump_directory":"d"}})",
           App::Scanner,
           "diagnostics.dump_directory: is not used by this application");
  rejected(
      R"({"version":1,"source":{"sync_config":"rig.json","color_codec":"mjpeg"},"diagnostics":{"dump_sets":1,"dump_directory":"d"}})",
      App::RigViewer, "diagnostics: is not used by this application");
}

// A CalibRig scenario: a rig's sync file and these further source fields, a
// session root, and these further sections; each addition leads with a comma.
std::string calib_rig(const std::string& source = "",
                      const std::string& sections = "") {
  return R"({"version":1,"source":{"sync_config":"rig.json")" + source +
         R"(},"output":{"path":"sessions"})" + sections + "}";
}

TEST(Config, CalibRigOpensItsRigAtThePresetAndWritesUnderTheSessionRoot) {
  const auto rig = scenario(calib_rig(), App::CalibRig, "/scenario");
  ASSERT_TRUE(rig) << rig.status().message();
  EXPECT_EQ(rig->operation, Operation::Calibrate);
  EXPECT_EQ(rig->source.kind, SourceKind::Live);
  EXPECT_EQ(rig->source.driver, "orbbec");
  EXPECT_EQ(rig->source.sync_config, "/scenario/rig.json");
  // The preset: 4K MJPEG colour and 640x576 depth at 25 fps.
  EXPECT_EQ(rig->source.color_codec, ColorCodec::Mjpeg);
  EXPECT_EQ(rig->source.color_width, 3840U);
  EXPECT_EQ(rig->source.color_height, 2160U);
  EXPECT_EQ(rig->source.depth_width, 640U);
  EXPECT_EQ(rig->source.depth_height, 576U);
  EXPECT_EQ(rig->source.fps, 25U);
  EXPECT_EQ(rig->output.path, "/scenario/sessions");
  EXPECT_EQ(rig->window.width, 1280);
  EXPECT_EQ(rig->window.height, 720);
  EXPECT_FALSE(rig->window.validation);
  // The rig is the sync file's, of any number of cameras.
  EXPECT_TRUE(rig->source.serial.empty());
  EXPECT_TRUE(rig->source.calibration.empty());
  EXPECT_FALSE(rig->source.apply_sync);

  // The scenario may state other modes; the app holds every camera to them.
  const auto stated = scenario(
      calib_rig(
          R"(,"driver":"orbbec","color_codec":"mjpeg","color_width":1920,"color_height":1080,"depth_width":320,"depth_height":288,"fps":15)",
          R"(,"window":{"width":2560,"height":1440,"validation":true})"),
      App::CalibRig, "/scenario");
  ASSERT_TRUE(stated) << stated.status().message();
  EXPECT_EQ(stated->source.color_codec, ColorCodec::Mjpeg);
  EXPECT_EQ(stated->source.color_width, 1920U);
  EXPECT_EQ(stated->source.color_height, 1080U);
  EXPECT_EQ(stated->source.depth_width, 320U);
  EXPECT_EQ(stated->source.depth_height, 288U);
  EXPECT_EQ(stated->source.fps, 15U);
  EXPECT_EQ(stated->window.width, 2560);
  EXPECT_EQ(stated->window.height, 1440);
  EXPECT_TRUE(stated->window.validation);

  // Both paths are the scenario's: relative to its directory, or absolute.
  const auto paths = scenario(
      R"({"version":1,"source":{"sync_config":"../rigs/./one.json"},"output":{"path":"/data/calib/../sessions"}})",
      App::CalibRig, "/scenario/config");
  ASSERT_TRUE(paths) << paths.status().message();
  EXPECT_EQ(paths->source.sync_config, "/scenario/rigs/one.json");
  EXPECT_EQ(paths->output.path, "/data/sessions");
  const auto under = scenario(
      R"({"version":1,"source":{"sync_config":"/rigs/three.json"},"output":{"path":"../sessions"}})",
      App::CalibRig, "/scenario/config");
  ASSERT_TRUE(under) << under.status().message();
  EXPECT_EQ(under->source.sync_config, "/rigs/three.json");
  EXPECT_EQ(under->output.path, "/scenario/sessions");
}

TEST(Config, CalibRigNeedsItsRigAndSessionRootAndRefusesWhatItDoesNotUse) {
  const auto refused = [](const std::string& text, const std::string& reason) {
    rejected(text, App::CalibRig, reason);
  };
  // The rig and the session root have no default.
  refused(R"({"version":1})", "source: is required");
  refused(R"({"version":1,"source":{"driver":"orbbec"},"output":{"path":"s"}})",
          "source.sync_config: is required");
  refused(R"({"version":1,"source":{"sync_config":""},"output":{"path":"s"}})",
          "source.sync_config: must not be empty");
  for (const char* output :
       {"", R"(,"output":{})", R"(,"output":{"path":""})"}) {
    refused(std::string(R"({"version":1,"source":{"sync_config":"rig.json"})") +
                output + "}",
            "output.path: is required");
  }

  // A capture saves each camera's JPEG, so colour crosses the wire as MJPEG.
  refused(calib_rig(R"(,"color_codec":"hevc")"),
          "source.color_codec: must be mjpeg: a capture saves the camera's "
          "JPEG");
  refused(calib_rig(R"(,"color_codec":"h264")"),
          "source.color_codec: unknown choice 'h264'");
  refused(calib_rig(R"(,"driver":"arkit")"),
          "source.driver: only orbbec is supported");
  // The live profiles' mode rules hold.
  refused(calib_rig(R"(,"color_width":3840)"),
          "source: color_width and color_height must be provided together");
  refused(calib_rig(R"(,"depth_height":576)"),
          "source: depth_width and depth_height must be provided together");
  refused(calib_rig(R"(,"color_width":0,"color_height":2160)"),
          "source.color_width: must be finite and positive");
  refused(calib_rig(R"(,"depth_width":640,"depth_height":-1)"),
          "source.depth_height: integer is out of range");
  refused(calib_rig(R"(,"fps":0)"), "source.fps: must be finite and positive");
  refused(calib_rig(R"(,"fps":25.0)"), "source.fps: expected an integer");

  // Source fields of the other profiles: one camera by serial, poses, writing
  // the sync roles, host clocks, datasets and meshes.
  for (const char* field :
       {R"("serial":"CL2A141000N")", R"("calibration":"poses.json")",
        R"("apply_sync":false)", R"("host_clock":false)", R"("path":"scene")",
        R"("format":"replica")", R"("camera_parameters":"cam.json")",
        R"("preload":false)", R"("height_m":1.7)", R"("up_axis":"y")",
        R"("sdf_mode":"signed")", R"("allow_holes":false)"}) {
    const std::string_view text(field);
    const std::string key(text.substr(1, text.find('"', 1) - 1));
    refused(calib_rig(std::string(",") + field),
            "source." + key + ": is not used by this application");
  }
  // Sections it has no use for, even empty.
  for (const char* section :
       {"fusion", "codec", "render", "run", "calibration", "diagnostics"}) {
    refused(calib_rig("", std::string(",\"") + section + "\":{}"),
            std::string(section) + ": is not used by this application");
  }
  // Its controls and marks are its ImGui overlay, which cannot be turned off.
  for (const char* overlay : {"true", "false"}) {
    refused(
        calib_rig("", std::string(R"(,"window":{"overlay":)") + overlay + "}"),
        "window.overlay: is not used by this application");
  }
  for (const char* key : {"prefix", "csv", "stream", "screenshot"}) {
    refused(
        std::string(
            R"({"version":1,"source":{"sync_config":"rig.json"},"output":{"path":"s",")") +
            key + R"(":"out"}})",
        std::string("output.") + key + ": is not used by this application");
  }

  // Calibrate is the rig calibrations' operation, and only theirs.
  for (const char* operation : {"view", "stream", "fuse", "project", "codec"}) {
    refused(
        std::string(R"({"version":1,"operation":")") + operation +
            R"(","source":{"sync_config":"rig.json"},"output":{"path":"s"}})",
        "operation: is not supported by this application");
  }
  refused(
      R"({"version":1,"source":{"kind":"dataset","sync_config":"rig.json"},"output":{"path":"s"}})",
      "source.kind: is not supported by this application");
  rejected(
      R"({"version":1,"operation":"calibrate","source":{"sync_config":"rig.json"}})",
      App::RigViewer, "operation: is not supported by this application");
  rejected(R"({"version":1,"operation":"calibrate"})", App::FuseOrbbec,
           "operation: is not supported by this application");
  rejected(R"({"version":1,"operation":"calibrate"})", App::GfxImage,
           "operation: is not supported by this application");
}

// A CalibColor scenario with no exposure or gain: a rig's sync file and
// calibration and these further source fields, a results root, and these
// further sections; each addition leads with a comma.
std::string calib_color_without_controls(const std::string& source,
                                         const std::string& sections = "") {
  return R"({"version":1,"source":{"sync_config":"rig.json","calibration":"rig_calibration.json")" +
         source + R"(},"output":{"path":"results"})" + sections + "}";
}
// The same at a stated exposure and gain.
std::string calib_color(const std::string& source = "",
                        const std::string& sections = "") {
  return calib_color_without_controls(
      R"(,"color_exposure":200,"color_gain":32)" + source, sections);
}

TEST(Config, CalibColorFitsItsRigAtAStatedExposureOnH265) {
  const auto fit = scenario(calib_color(), App::CalibColor, "/scenario");
  ASSERT_TRUE(fit) << fit.status().message();
  EXPECT_EQ(fit->operation, Operation::Calibrate);
  EXPECT_EQ(fit->source.kind, SourceKind::Live);
  EXPECT_EQ(fit->source.driver, "orbbec");
  EXPECT_EQ(fit->source.sync_config, "/scenario/rig.json");
  EXPECT_EQ(fit->source.calibration, "/scenario/rig_calibration.json");
  // The Orbbec profiles' modes: H.265 colour at 3840x2160, depth at 640x576,
  // 25 fps.
  EXPECT_EQ(fit->source.color_codec, ColorCodec::Hevc);
  EXPECT_EQ(fit->source.color_width, 3840U);
  EXPECT_EQ(fit->source.color_height, 2160U);
  EXPECT_EQ(fit->source.depth_width, 640U);
  EXPECT_EQ(fit->source.depth_height, 576U);
  EXPECT_EQ(fit->source.fps, 25U);
  EXPECT_EQ(fit->source.color_exposure, 200U);
  EXPECT_EQ(fit->source.color_gain, 32U);
  // Two seconds of sets, written under the results root.
  EXPECT_EQ(fit->run.frames, 50);
  EXPECT_EQ(fit->output.path, "/scenario/results");
  // The rig is the sync file's, of any number of cameras.
  EXPECT_TRUE(fit->source.serial.empty());
  EXPECT_FALSE(fit->source.apply_sync);
  // No other profile has an exposure or gain to set.
  EXPECT_EQ(Source{}.color_exposure, 0U);
  EXPECT_EQ(Source{}.color_gain, 0U);

  // Stated modes, controls and count; the camera checks the controls' range
  // when it opens.
  const auto stated = scenario(
      R"({"version":1,"operation":"calibrate","source":{"kind":"live","driver":"orbbec","sync_config":"/rigs/two.json","calibration":"../calib/./rig.json","color_codec":"hevc","color_width":1920,"color_height":1080,"depth_width":320,"depth_height":288,"fps":15,"color_exposure":1,"color_gain":4294967295},"run":{"frames":1},"output":{"path":"/data/results/../fits"}})",
      App::CalibColor, "/scenario/config");
  ASSERT_TRUE(stated) << stated.status().message();
  EXPECT_EQ(stated->source.sync_config, "/rigs/two.json");
  EXPECT_EQ(stated->source.calibration, "/scenario/calib/rig.json");
  EXPECT_EQ(stated->source.color_codec, ColorCodec::Hevc);
  EXPECT_EQ(stated->source.color_width, 1920U);
  EXPECT_EQ(stated->source.color_height, 1080U);
  EXPECT_EQ(stated->source.depth_width, 320U);
  EXPECT_EQ(stated->source.depth_height, 288U);
  EXPECT_EQ(stated->source.fps, 15U);
  EXPECT_EQ(stated->source.color_exposure, 1U);
  EXPECT_EQ(stated->source.color_gain, 4294967295U);
  EXPECT_EQ(stated->run.frames, 1);
  EXPECT_EQ(stated->output.path, "/data/fits");
}

TEST(Config, CalibColorNeedsItsRigCalibrationControlsAndResultsRoot) {
  const auto refused = [](const std::string& text, const std::string& reason) {
    rejected(text, App::CalibColor, reason);
  };
  // The rig, its calibration, the controls and the results root have no
  // default.
  refused(R"({"version":1})", "source: is required");
  refused(
      R"({"version":1,"source":{"calibration":"c.json","color_exposure":200,"color_gain":32},"output":{"path":"r"}})",
      "source.sync_config: is required");
  refused(
      R"({"version":1,"source":{"sync_config":"rig.json","color_exposure":200,"color_gain":32},"output":{"path":"r"}})",
      "source.calibration: is required");
  refused(
      R"({"version":1,"source":{"sync_config":"rig.json","calibration":"","color_exposure":200,"color_gain":32},"output":{"path":"r"}})",
      "source.calibration: must not be empty");
  for (const char* output :
       {"", R"(,"output":{})", R"(,"output":{"path":""})"}) {
    refused(
        std::string(
            R"({"version":1,"source":{"sync_config":"rig.json","calibration":"c.json","color_exposure":200,"color_gain":32})") +
            output + "}",
        "output.path: is required");
  }
  refused(calib_color_without_controls(R"(,"color_gain":32)"),
          "source.color_exposure: is required");
  refused(calib_color_without_controls(R"(,"color_exposure":200)"),
          "source.color_gain: is required");
  // A manual exposure and gain: positive integers.
  for (const auto& [value, reason] :
       {std::pair{"0", "must be finite and positive"},
        std::pair{"-1", "integer is out of range"},
        std::pair{"4294967296", "integer is out of range"},
        std::pair{"1.5", "expected an integer"},
        std::pair{"200.0", "expected an integer"},
        std::pair{R"("200")", "expected an integer"},
        std::pair{"true", "expected an integer"},
        std::pair{"null", "expected an integer"}}) {
    refused(calib_color_without_controls(std::string(R"(,"color_exposure":)") +
                                         value + R"(,"color_gain":32)"),
            std::string("source.color_exposure: ") + reason);
    refused(calib_color_without_controls(
                R"(,"color_exposure":200,"color_gain":)" + std::string(value)),
            std::string("source.color_gain: ") + reason);
  }

  // The fit sees the colour reconstruction decodes, so colour crosses the
  // wire as H.265.
  refused(calib_color(R"(,"color_codec":"mjpeg")"),
          "source.color_codec: must be hevc: the fit sees the colour "
          "reconstruction decodes");
  refused(calib_color(R"(,"color_codec":"h264")"),
          "source.color_codec: unknown choice 'h264'");
  refused(calib_color(R"(,"driver":"arkit")"),
          "source.driver: only orbbec is supported");
  // The live profiles' mode rules hold.
  refused(calib_color(R"(,"color_height":2160)"),
          "source: color_width and color_height must be provided together");
  refused(calib_color(R"(,"depth_width":0,"depth_height":576)"),
          "source.depth_width: must be finite and positive");
  refused(calib_color(R"(,"fps":0)"),
          "source.fps: must be finite and positive");
  // A positive count of sets.
  for (const char* frames : {"0", "-1"}) {
    refused(calib_color("", std::string(R"(,"run":{"frames":)") + frames + "}"),
            "run.frames: must be positive");
  }
  refused(calib_color("", R"(,"run":{"frames":2.5})"),
          "run.frames: expected an integer");
  for (const char* key : {"sets", "first"}) {
    refused(calib_color("", std::string(R"(,"run":{")") + key + R"(":1})"),
            std::string("run.") + key + ": is not used by this application");
  }
  // A run never overwrites the rig files it reads.
  for (const auto& [path, source] :
       {std::pair{"./rig.json", "sync_config"},
        std::pair{"/scenario/rig_calibration.json", "calibration"}}) {
    refused(
        std::string(
            R"({"version":1,"source":{"sync_config":"rig.json","calibration":"rig_calibration.json","color_exposure":200,"color_gain":32},"output":{"path":")") +
            path + R"("}})",
        std::string("output.path: must differ from source.") + source);
  }

  // Source fields of the other profiles: one camera by serial, writing the
  // sync roles, host clocks, datasets, meshes and streams.
  for (const char* field :
       {R"("serial":"CL2A141000N")", R"("apply_sync":false)",
        R"("host_clock":false)", R"("path":"scene")", R"("format":"replica")",
        R"("camera_parameters":"cam.json")", R"("preload":false)",
        R"("height_m":1.7)", R"("metres_per_unit":1)", R"("up_axis":"y")",
        R"("up_vector":[0,1,0])", R"("sdf_mode":"signed")",
        R"("shell_voxels":1.5)", R"("allow_holes":false)", R"("host":"lab")",
        R"("port":0)"}) {
    const std::string_view text(field);
    const std::string key(text.substr(1, text.find('"', 1) - 1));
    refused(calib_color(std::string(",") + field),
            "source." + key + ": is not used by this application");
  }
  // Sections it has no use for, even empty: it opens no window.
  for (const char* section :
       {"fusion", "codec", "window", "render", "calibration", "diagnostics"}) {
    refused(calib_color("", std::string(",\"") + section + "\":{}"),
            std::string(section) + ": is not used by this application");
  }
  // Its one output is the results root.
  for (const char* member :
       {R"("prefix":"out")", R"("csv":"out")", R"("stream":"out")",
        R"("screenshot":"out")", R"("serve":0)"}) {
    const std::string_view text(member);
    const std::string key(text.substr(1, text.find('"', 1) - 1));
    refused(
        std::string(
            R"({"version":1,"source":{"sync_config":"rig.json","calibration":"c.json","color_exposure":200,"color_gain":32},"output":{"path":"r",)") +
            member + "}}",
        "output." + key + ": is not used by this application");
  }

  // Calibrate, from a live source.
  for (const char* operation : {"view", "stream", "fuse", "project", "codec"}) {
    refused(std::string(R"({"operation":")") + operation + R"(",)" +
                calib_color().substr(1),
            "operation: is not supported by this application");
  }
  refused(
      R"({"version":1,"source":{"kind":"dataset","sync_config":"rig.json","calibration":"c.json","color_exposure":200,"color_gain":32},"output":{"path":"r"}})",
      "source.kind: is not supported by this application");
}

TEST(Config, EachProfileKeepsItsOwnOrbbecSourceFields) {
  // RigViewer opens its rig from a sync file, may write the sync roles and
  // load poses, and states modes other than the defaults.
  const auto rig = scenario(
      R"({"version":1,"source":{"sync_config":"rig.json","calibration":"poses.json","apply_sync":true,"color_codec":"hevc","color_width":1280,"color_height":720,"depth_width":320,"depth_height":288,"fps":30}})",
      App::RigViewer, "/scenario");
  ASSERT_TRUE(rig) << rig.status().message();
  EXPECT_EQ(rig->source.sync_config, "/scenario/rig.json");
  EXPECT_EQ(rig->source.calibration, "/scenario/poses.json");
  EXPECT_TRUE(rig->source.apply_sync);
  EXPECT_EQ(rig->source.color_codec, ColorCodec::Hevc);
  EXPECT_EQ(rig->source.color_width, 1280U);
  EXPECT_EQ(rig->source.color_height, 720U);
  EXPECT_EQ(rig->source.depth_width, 320U);
  EXPECT_EQ(rig->source.depth_height, 288U);
  EXPECT_EQ(rig->source.fps, 30U);
  // FuseOrbbec may open one camera by serial, on the host clock. A mode it
  // does not state keeps its default.
  const auto one = scenario(
      R"({"version":1,"source":{"serial":"CL2A141000N","calibration":"poses.json","host_clock":true,"color_codec":"mjpeg","fps":5}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(one) << one.status().message();
  EXPECT_EQ(one->source.serial, "CL2A141000N");
  EXPECT_EQ(one->source.calibration, "/scenario/poses.json");
  EXPECT_TRUE(one->source.host_clock);
  EXPECT_EQ(one->source.color_codec, ColorCodec::Mjpeg);
  EXPECT_EQ(one->source.color_width, 3840U);
  EXPECT_EQ(one->source.color_height, 2160U);
  EXPECT_EQ(one->source.depth_width, 640U);
  EXPECT_EQ(one->source.depth_height, 576U);
  EXPECT_EQ(one->source.fps, 5U);
  const auto sized = scenario(
      R"({"version":1,"source":{"color_width":1920,"color_height":1080,"depth_width":320,"depth_height":288,"fps":15}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(sized) << sized.status().message();
  EXPECT_EQ(sized->source.color_codec, ColorCodec::Hevc);
  EXPECT_EQ(sized->source.color_width, 1920U);
  EXPECT_EQ(sized->source.color_height, 1080U);
  EXPECT_EQ(sized->source.depth_width, 320U);
  EXPECT_EQ(sized->source.depth_height, 288U);
  EXPECT_EQ(sized->source.fps, 15U);
  // Scanner takes only the kind and driver, and CalibProjectPoint no fusion.
  for (const char* field :
       {R"("sync_config":"rig.json")", R"("calibration":"poses.json")",
        R"("color_codec":"mjpeg")", R"("fps":30)"}) {
    const std::string_view text(field);
    const std::string key(text.substr(1, text.find('"', 1) - 1));
    rejected(std::string(R"({"version":1,"source":{)") + field + "}}",
             App::Scanner,
             "source." + key + ": is not used by this application");
  }
  rejected(R"({"version":1,"fusion":{}})", App::CalibProjectPoint,
           "fusion: is not used by this application");
}

TEST(Config, FuseRealsenseFusesOneCameraAtItsModesToAPly) {
  const auto fuse =
      scenario(R"({"version":1})", App::FuseRealsense, "/scenario");
  ASSERT_TRUE(fuse) << fuse.status().message();
  EXPECT_EQ(fuse->operation, Operation::Fuse);
  EXPECT_EQ(fuse->source.kind, SourceKind::Live);
  EXPECT_EQ(fuse->source.driver, "realsense");
  EXPECT_TRUE(fuse->source.serial.empty());
  EXPECT_EQ(fuse->source.color_width, 1280U);
  EXPECT_EQ(fuse->source.color_height, 800U);
  EXPECT_EQ(fuse->source.depth_width, 1280U);
  EXPECT_EQ(fuse->source.depth_height, 720U);
  EXPECT_EQ(fuse->source.fps, 30U);
  EXPECT_EQ(fuse->fusion.voxel_size_m, 0.02F);
  EXPECT_EQ(fuse->fusion.truncation_m, 0.08F);
  EXPECT_EQ(fuse->fusion.min_depth_m, 0.25F);
  EXPECT_EQ(fuse->fusion.max_depth_m, 5.F);
  EXPECT_TRUE(fuse->fusion.dynamic);
  EXPECT_EQ(fuse->fusion.free_after, 30);
  EXPECT_EQ(fuse->run.frames, 300);
  EXPECT_EQ(fuse->output.path, "/scenario/fuse_realsense.ply");
  EXPECT_TRUE(fuse->output.stream.empty());
  EXPECT_EQ(fuse->output.serve, 0U);

  // Every field it takes, each away from its default.
  const auto set = scenario(
      R"({"version":1,"source":{"driver":"realsense","serial":"123456789012","color_width":640,"color_height":480,"depth_width":848,"depth_height":480,"fps":15},"fusion":{"voxel_size_m":0.01,"truncation_m":0.05,"min_depth_m":0.5,"max_depth_m":4,"max_weight":40,"dynamic":false,"free_after":0},"output":{"path":"out/room.ply"},"run":{"frames":900}})",
      App::FuseRealsense, "/scenario");
  ASSERT_TRUE(set) << set.status().message();
  EXPECT_EQ(set->source.serial, "123456789012");
  EXPECT_EQ(set->source.color_width, 640U);
  EXPECT_EQ(set->source.color_height, 480U);
  EXPECT_EQ(set->source.depth_width, 848U);
  EXPECT_EQ(set->source.depth_height, 480U);
  EXPECT_EQ(set->source.fps, 15U);
  EXPECT_EQ(set->fusion.voxel_size_m, 0.01F);
  EXPECT_EQ(set->fusion.truncation_m, 0.05F);
  EXPECT_EQ(set->fusion.min_depth_m, 0.5F);
  EXPECT_EQ(set->fusion.max_depth_m, 4.F);
  EXPECT_EQ(set->fusion.max_weight, 40.F);
  EXPECT_FALSE(set->fusion.dynamic);
  EXPECT_EQ(set->fusion.free_after, 0);
  EXPECT_EQ(set->output.path, "/scenario/out/room.ply");
  EXPECT_EQ(set->run.frames, 900);

  // Its driver alone, and FuseOrbbec's not it.
  rejected(R"({"version":1,"source":{"driver":"orbbec"}})", App::FuseRealsense,
           "source.driver: only realsense is supported");
  rejected(R"({"version":1,"source":{"driver":"realsense"}})", App::FuseOrbbec,
           "source.driver: only orbbec is supported");
  rejected(R"({"version":1,"operation":"stream"})", App::FuseRealsense,
           "operation: is not supported by this application");
  // The Orbbec rig's fields, the codec and its outputs, and diagnostics are
  // not its.
  for (const char* field :
       {R"("source":{"sync_config":"rig.json"})",
        R"("source":{"apply_sync":true})",
        R"("source":{"calibration":"poses.json"})",
        R"("source":{"host_clock":true})",
        R"("source":{"color_codec":"mjpeg"})",
        R"("source":{"color_exposure":100})", R"("codec":{})",
        R"("output":{"stream":"out.vrst"})", R"("output":{"csv":"out.csv"})",
        R"("output":{"serve":9000})", R"("diagnostics":{"readback":true})",
        R"("fusion":{"occlusion_m":0.1})", R"("window":{})"}) {
    rejected(std::string(R"({"version":1,)") + field + "}", App::FuseRealsense,
             "is not used by this application");
  }
  // Its modes and counts are checked as the Orbbec profiles' are.
  rejected(R"({"version":1,"source":{"color_width":1280}})", App::FuseRealsense,
           "source: color_width and color_height must be provided together");
  rejected(R"({"version":1,"source":{"depth_width":0,"depth_height":720}})",
           App::FuseRealsense,
           "source.depth_width: must be finite and positive");
  rejected(R"({"version":1,"source":{"fps":0}})", App::FuseRealsense,
           "source.fps: must be finite and positive");
  rejected(R"({"version":1,"run":{"frames":0}})", App::FuseRealsense,
           "run.frames: must be positive");
  rejected(R"({"version":1,"output":{"path":""}})", App::FuseRealsense,
           "output.path: is required");
  rejected(R"({"version":1,"fusion":{"min_depth_m":5,"max_depth_m":4}})",
           App::FuseRealsense, "min_depth_m");
  const auto unsourced = parse(R"({"version":1,"operation":"fuse"})",
                               App::FuseRealsense, "/scenario");
  ASSERT_FALSE(unsourced);
  EXPECT_NE(unsourced.status().message().find("source: is required"),
            std::string::npos)
      << unsourced.status().message();
}

TEST(Config, LiveRelationsAndCountsAreCentral) {
  rejected(R"({"version":1,"source":{"serial":"A\u0000B"}})", App::FuseOrbbec,
           "source.serial: embedded NUL");
  rejected(R"({"version":1,"source":{"serial":"A","sync_config":"rig.json"}})",
           App::FuseOrbbec,
           "source.serial: cannot be combined with source.sync_config");
  rejected(R"({"version":1,"source":{"apply_sync":true}})", App::FuseOrbbec,
           "source.apply_sync: requires source.sync_config");
  rejected(
      R"({"version":1,"source":{"host_clock":true,"sync_config":"rig.json"}})",
      App::FuseOrbbec,
      "source.host_clock: cannot be combined with source.sync_config");
  rejected(R"({"version":1,"source":{"color_width":3840}})", App::FuseOrbbec,
           "source: color_width and color_height must be provided together");
  rejected(R"({"version":1,"source":{"depth_width":640,"depth_height":0}})",
           App::FuseOrbbec, "source.depth_height: must be finite and positive");
  rejected(R"({"version":1,"source":{"fps":-1}})", App::FuseOrbbec,
           "source.fps: integer is out of range");
  rejected(R"({"version":1,"source":{"fps":4294967296}})", App::FuseOrbbec,
           "source.fps: integer is out of range");
  rejected(R"({"version":1,"run":{"frames":2147483648}})", App::FuseOrbbec,
           "run.frames: integer is out of range");
  rejected(R"({"version":1,"run":{"frames":0}})", App::FuseOrbbec,
           "run.frames: must be positive");
}

TEST(Config, FusionDefaultsAndBoundsDoNotDependOnTheConsumer) {
  for (const auto* const trunc :
       {"", ",\"truncation_m\":0", ",\"truncation_m\":-1"}) {
    const auto parsed =
        scenario(std::string(R"({"version":1,"fusion":{"voxel_size_m":0.03)") +
                     trunc + "}}",
                 App::FuseOrbbec);
    ASSERT_TRUE(parsed) << parsed.status().message();
    EXPECT_EQ(parsed->fusion.truncation_m, 4.F * 0.03F);
  }
  rejected(R"({"version":1,"fusion":{"voxel_size_m":1e38}})", App::FuseOrbbec,
           "voxel_size_m");
  rejected(R"({"version":1,"fusion":{"min_depth_m":5,"max_depth_m":4}})",
           App::FuseOrbbec, "min_depth_m");
  rejected(R"({"version":1,"fusion":{"max_weight":0}})", App::FuseOrbbec,
           "max_weight");
  rejected(
      R"({"version":1,"source":{"path":"scene"},"fusion":{"buckets":268435456}})",
      App::FuseReplica, "fusion.buckets");
}

TEST(Config, JsonParsingRestoresCallerRoundingOnSuccessAndFailure) {
  struct RoundingRestore {
    int mode = std::fegetround();
    ~RoundingRestore() { std::fesetround(mode); }
  };
  const RoundingRestore restore;
  for (const int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD}) {
    ASSERT_EQ(std::fesetround(mode), 0);
    const auto parsed = scenario(
        R"({"version":1,"calibration":{"distortion":[1e-999,-1e-999,0,0,0],"point":[1.7976931348623157e308,0,1]}})",
        App::CalibProjectPoint);
    ASSERT_TRUE(parsed) << parsed.status().message();
    EXPECT_EQ(std::fegetround(), mode);
    EXPECT_EQ(parsed->calibration.distortion[0], 0.);
    EXPECT_EQ(parsed->calibration.distortion[1], 0.);
    EXPECT_EQ(parsed->calibration.point[0], std::numeric_limits<double>::max());
    for (const auto* const token :
         {"1e999", "-1e999", "1.25e999", "-1.25e999"}) {
      rejected(std::string(R"({"version":1,"calibration":{"point":[0,0,)") +
                   token + "]}}",
               App::CalibProjectPoint, "overflow");
      EXPECT_EQ(std::fegetround(), mode);
    }
    EXPECT_FALSE(parse("{", App::GfxModel));
    EXPECT_EQ(std::fegetround(), mode);
    rejected(R"({"version":1,"version":1})", App::GfxModel, "duplicate");
    EXPECT_EQ(std::fegetround(), mode);
    rejected(R"({"version":1,"fusion":{"voxel_size_m":3.5e38}})",
             App::FuseOrbbec, "fusion.voxel_size_m");
    EXPECT_EQ(std::fegetround(), mode);
  }
}

TEST(Config, CodecSettingsKeepGenericChecksAndLeaveStepsToTheCodec) {
  const auto parsed = scenario(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_table":"radial","coefficient_count":512,"quantization_scale":0.2}})",
      App::CodecReplica);
  ASSERT_TRUE(parsed) << parsed.status().message();
  EXPECT_EQ(parsed->codec.quantization_table, QuantizationTable::Radial);
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_table":"steep"}})",
      App::CodecReplica, "codec.quantization_table");
  // The codec codes on the device alone, so there is no entropy choice left.
  for (const char* entropy : {"auto", "host", "device"}) {
    rejected(
        std::string(
            R"({"version":1,"source":{"path":"scene"},"codec":{"entropy":")") +
            entropy + "\"}}",
        App::CodecReplica, "codec.entropy: unknown field");
    rejected(
        std::string(
            R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"},"codec":{"entropy":")") +
            entropy + "\"}}",
        App::CodecMesh, "codec.entropy: unknown field");
  }
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"coefficient_count":513}})",
      App::CodecReplica, "coefficient_count");
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_scale":1e-40}})",
      App::CodecReplica, "quantization_scale");
  // Effective steps outside the codec's bounds are the codec's to refuse.
  const auto fine = scenario(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_scale":0.0001}})",
      App::CodecReplica);
  EXPECT_TRUE(fine) << fine.status().message();
  const auto coarse = scenario(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_table":"radial","coefficient_count":512,"quantization_scale":4}})",
      App::CodecReplica);
  EXPECT_TRUE(coarse) << coarse.status().message();
}

// A scenario for codec profile `app` whose codec section is `fields`, or that
// has no codec section when `fields` is empty.
std::string codec_scenario(App app, const std::string& fields) {
  std::string text =
      app == App::CodecMesh
          ? R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"})"
          : R"({"version":1,"source":{"path":"scene"})";
  if (!fields.empty()) text += R"(,"codec":{)" + fields + "}";
  return text + "}";
}

TEST(Config, IntraPeriodIsAPositiveCountForBothCodecs) {
  EXPECT_EQ(Codec{}.intra_period, 1U);
  const auto with_period = [](App app, const std::string& period) {
    return codec_scenario(app, R"("intra_period":)" + period);
  };
  for (const auto app : {App::CodecReplica, App::CodecMesh}) {
    const auto omitted = scenario(codec_scenario(app, ""), app);
    ASSERT_TRUE(omitted) << omitted.status().message();
    EXPECT_EQ(omitted->codec.intra_period, 1U);
    for (const std::uint32_t period : {1U, 30U, 4294967295U}) {
      const auto parsed =
          scenario(with_period(app, std::to_string(period)), app);
      ASSERT_TRUE(parsed) << parsed.status().message();
      EXPECT_EQ(parsed->codec.intra_period, period);
    }
    rejected(with_period(app, "0"), app,
             "codec.intra_period: must be finite and positive");
    for (const char* period : {"-1", "4294967296"}) {
      rejected(with_period(app, period), app,
               "codec.intra_period: integer is out of range");
    }
    for (const char* period : {"1.5", "8.0", R"("8")", "true", "null", "[8]"}) {
      rejected(with_period(app, period), app,
               "codec.intra_period: expected an integer");
    }
  }
  // The codec section, and so the period, belongs to the codec profiles.
  rejected(with_period(App::CodecReplica, "8"), App::FuseReplica,
           "codec: is not used by this application");
  rejected(R"({"version":1,"codec":{"intra_period":8}})", App::GfxModel,
           "codec: is not used by this application");
  // Inspection codes nothing, so an enabled inspect_only refuses a period,
  // even the default; a disabled one does not.
  const auto coded =
      scenario(codec_scenario(App::CodecMesh,
                              R"("inspect_only":false,"intra_period":8)"),
               App::CodecMesh);
  ASSERT_TRUE(coded) << coded.status().message();
  EXPECT_EQ(coded->codec.intra_period, 8U);
  for (const char* period : {"1", "8"}) {
    rejected(
        codec_scenario(
            App::CodecMesh,
            std::string(R"("inspect_only":true,"intra_period":)") + period),
        App::CodecMesh,
        "codec.intra_period: is not used when codec.inspect_only is "
        "enabled");
  }
}

TEST(Config, UpdateSizeIsAnyCountWhereThereArePFrames) {
  EXPECT_EQ(Codec{}.update_size, 0U);
  const auto with_size = [](App app, const std::string& size) {
    return codec_scenario(app, R"("intra_period":30,"update_size":)" + size);
  };
  for (const auto app : {App::CodecReplica, App::CodecMesh}) {
    const auto omitted = scenario(codec_scenario(app, ""), app);
    ASSERT_TRUE(omitted) << omitted.status().message();
    EXPECT_EQ(omitted->codec.update_size, 0U);
    // Core admits any u32, 0 selecting the codec's own size; the codec
    // refuses one outside its range.
    for (const std::uint32_t size : {0U, 1U, 64U, 1024U, 4294967295U}) {
      const auto parsed = scenario(with_size(app, std::to_string(size)), app);
      ASSERT_TRUE(parsed) << parsed.status().message();
      EXPECT_EQ(parsed->codec.update_size, size);
      EXPECT_EQ(parsed->codec.intra_period, 30U);
      EXPECT_EQ(parsed->codec.segment_size, Codec{}.segment_size);
    }
    for (const char* size : {"-1", "4294967296"}) {
      rejected(with_size(app, size), app,
               "codec.update_size: integer is out of range");
    }
    for (const char* size :
         {"1.5", "64.0", R"("64")", "true", "null", "[64]"}) {
      rejected(with_size(app, size), app,
               "codec.update_size: expected an integer");
    }
    // A period of 1, explicit or the default, codes no P-frame, so it refuses
    // a size, even 0.
    for (const char* fields :
         {R"("update_size":64)", R"("intra_period":1,"update_size":0)"}) {
      rejected(codec_scenario(app, fields), app,
               "codec.update_size: is not used when codec.intra_period is 1");
    }
  }
  rejected(with_size(App::CodecReplica, "64"), App::FuseReplica,
           "codec: is not used by this application");
  // Inspection codes nothing, so an enabled inspect_only refuses a size,
  // even the default; a disabled one does not.
  const auto coded =
      scenario(codec_scenario(
                   App::CodecMesh,
                   R"("inspect_only":false,"intra_period":8,"update_size":64)"),
               App::CodecMesh);
  ASSERT_TRUE(coded) << coded.status().message();
  EXPECT_EQ(coded->codec.update_size, 64U);
  for (const char* size : {"0", "64"}) {
    rejected(
        codec_scenario(
            App::CodecMesh,
            std::string(R"("inspect_only":true,"update_size":)") + size),
        App::CodecMesh,
        "codec.update_size: is not used when codec.inspect_only is enabled");
  }
}

// Every profile, so that one added later is held to each rule too.
std::vector<App> every_profile() {
  std::vector<App> apps;
  for (int app = 0; app <= static_cast<int>(App::FuseRealsense); ++app)
    apps.push_back(static_cast<App>(app));
  return apps;
}
// A scenario for `app` with the source it requires, if any, and these
// sections; each addition leads with a comma. CalibRig's and CalibColor's
// required output.path is left out: a refusal names an unsupported field
// first.
std::string with_sections(App app, const std::string& sections) {
  std::string source;
  if (app == App::FuseReplica || app == App::CodecReplica ||
      app == App::FuseRender || app == App::FuseViewer)
    source = R"(,"source":{"path":"scene"})";
  if (app == App::CodecMesh)
    source = R"(,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"})";
  if (app == App::RigViewer || app == App::CalibRig)
    source = R"(,"source":{"sync_config":"rig.json"})";
  if (app == App::CalibColor)
    source =
        R"(,"source":{"sync_config":"rig.json","calibration":"c.json","color_exposure":200,"color_gain":32})";
  if (app == App::Player) source = R"(,"source":{"path":"rig.vrst"})";
  return R"({"version":1)" + source + sections + "}";
}

TEST(Config, FuseOrbbecCodesEachSetWithTheCodecApplicationsKeys) {
  // Off by default, with the codec applications' codec defaults.
  const auto plain = scenario(R"({"version":1})", App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(plain) << plain.status().message();
  EXPECT_TRUE(plain->output.csv.empty());
  EXPECT_TRUE(plain->output.stream.empty());
  EXPECT_EQ(plain->output.path, "/scenario/fuse_orbbec.ply");
  for (const auto app : {App::CodecReplica, App::CodecMesh}) {
    const auto codec = scenario(codec_scenario(app, ""), app);
    ASSERT_TRUE(codec) << codec.status().message();
    EXPECT_EQ(plain->codec.coefficient_count, codec->codec.coefficient_count);
    EXPECT_EQ(plain->codec.quantization_scale, codec->codec.quantization_scale);
    EXPECT_EQ(plain->codec.quantization_table, codec->codec.quantization_table);
    EXPECT_EQ(plain->codec.segment_size, codec->codec.segment_size);
    EXPECT_EQ(plain->codec.update_size, codec->codec.update_size);
    EXPECT_EQ(plain->codec.intra_period, codec->codec.intra_period);
    EXPECT_EQ(plain->codec.sweep, codec->codec.sweep);
  }

  // Every codec key the codec applications share, and both outputs, which
  // resolve against the scenario.
  const auto coded = scenario(
      R"({"version":1,"source":{"sync_config":"rig.json","calibration":"poses.json"},"codec":{"coefficient_count":32,"quantization_scale":0.05,"quantization_table":"band","segment_size":128,"intra_period":30,"update_size":256,"sweep":true},"output":{"path":"rig.ply","csv":"runs/rig.csv","stream":"runs/../rig.vrst"}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(coded) << coded.status().message();
  EXPECT_EQ(coded->codec.coefficient_count, 32U);
  EXPECT_EQ(coded->codec.quantization_scale, 0.05F);
  EXPECT_EQ(coded->codec.quantization_table, QuantizationTable::Band);
  EXPECT_EQ(coded->codec.segment_size, 128U);
  EXPECT_EQ(coded->codec.intra_period, 30U);
  EXPECT_EQ(coded->codec.update_size, 256U);
  EXPECT_TRUE(coded->codec.sweep);
  EXPECT_EQ(coded->output.path, "/scenario/rig.ply");
  EXPECT_EQ(coded->output.csv, "/scenario/runs/rig.csv");
  EXPECT_EQ(coded->output.stream, "/scenario/rig.vrst");
  // Either output alone codes, at the codec's defaults when it has no section.
  const auto frames =
      scenario(R"({"version":1,"output":{"stream":"rig.vrst"}})",
               App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(frames) << frames.status().message();
  EXPECT_EQ(frames->output.stream, "/scenario/rig.vrst");
  EXPECT_EQ(frames->output.path, "/scenario/fuse_orbbec.ply");
  EXPECT_EQ(frames->codec.intra_period, 1U);
  const auto rows = scenario(
      R"({"version":1,"codec":{"intra_period":8},"output":{"csv":"rig.csv"}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(rows) << rows.status().message();
  EXPECT_EQ(rows->output.csv, "/scenario/rig.csv");
  EXPECT_TRUE(rows->output.stream.empty());
  EXPECT_EQ(rows->codec.intra_period, 8U);

  // A codec section without an output would code nothing.
  for (const char* output :
       {"", R"(,"output":{"path":"rig.ply"})",
        R"(,"output":{"csv":"","stream":""})", R"(,"output":{"serve":0})"}) {
    for (const char* codec : {"{}", R"({"intra_period":30})"}) {
      rejected(std::string(R"({"version":1,"codec":)") + codec + output + "}",
               App::FuseOrbbec,
               "codec: requires output.stream, output.csv or output.serve");
    }
  }
  // The codec applications' checks hold.
  const auto refused = [](const std::string& codec, const std::string& reason) {
    rejected(R"({"version":1,"codec":{)" + codec +
                 R"(},"output":{"stream":"rig.vrst"}})",
             App::FuseOrbbec, reason);
  };
  refused(R"("intra_period":0)",
          "codec.intra_period: must be finite and positive");
  refused(R"("update_size":64)",
          "codec.update_size: is not used when codec.intra_period is 1");
  refused(R"("coefficient_count":513)",
          "codec.coefficient_count: must be in [1, 512]");
  refused(R"("quantization_scale":1e-40)",
          "codec.quantization_scale: must be positive and normal");
  refused(R"("segment_size":0)",
          "codec.segment_size: must be finite and positive");
  refused(R"("quantization_table":"steep")",
          "codec.quantization_table: unknown choice 'steep'");
  // Every fused set is one coded frame, and each is coded: encode_every stays
  // CodecReplica's and inspect_only CodecMesh's, even at their defaults.
  refused(R"("encode_every":1)",
          "codec.encode_every: is not used by this application");
  refused(R"("inspect_only":false)",
          "codec.inspect_only: is not used by this application");
  rejected(R"({"version":1,"output":{"prefix":"runs/rig"}})", App::FuseOrbbec,
           "output.prefix: is not used by this application");

  // The PLY, the rows and the frames are three files, once resolved.
  for (const char* paths :
       {R"("csv":"out.bin","stream":"out.bin")",
        R"("csv":"runs/../out.bin","stream":"./out.bin")"}) {
    rejected(std::string(R"({"version":1,"output":{)") + paths + "}}",
             App::FuseOrbbec, "output.stream: must differ from output.csv");
  }
  for (const char* key : {"stream", "csv"}) {
    for (const char* paths :
         {R"("path":"rig.ply",")", R"("path":"/scenario/rig.ply",")"}) {
      rejected(std::string(R"({"version":1,"output":{)") + paths + key +
                   R"(":"./rig.ply"}})",
               App::FuseOrbbec,
               std::string("output.") + key + ": must differ from output.path");
    }
    // The PLY's default name counts too.
    rejected(std::string(R"({"version":1,"output":{")") + key +
                 R"(":"fuse_orbbec.ply"}})",
             App::FuseOrbbec,
             std::string("output.") + key + ": must differ from output.path");
  }

  // Only the codec profiles and FuseOrbbec code: every other profile refuses
  // the codec section, even empty, and both of its outputs.
  for (const auto app : every_profile()) {
    if (app == App::CodecReplica || app == App::CodecMesh ||
        app == App::FuseOrbbec)
      continue;
    rejected(with_sections(app, R"(,"codec":{})"), app,
             "codec: is not used by this application");
    const bool writes = app == App::FuseReplica || app == App::FuseRender ||
                        app == App::GfxModel || app == App::CalibRig ||
                        app == App::CalibColor || app == App::FuseRealsense;
    for (const char* key : {"csv", "stream"}) {
      rejected(with_sections(
                   app, std::string(R"(,"output":{")") + key + R"(":"out"})"),
               app,
               (writes ? std::string("output.") + key : std::string("output")) +
                   ": is not used by this application");
    }
  }
}

TEST(Config, OnlyFuseOrbbecServesItsCodedFramesLive) {
  // A port, 0 serving nothing by default.
  EXPECT_EQ(Output{}.serve, 0U);
  const auto plain = scenario(R"({"version":1})", App::FuseOrbbec);
  ASSERT_TRUE(plain) << plain.status().message();
  EXPECT_EQ(plain->output.serve, 0U);
  for (const unsigned port : {0U, 1U, 47801U, 65535U}) {
    const auto parsed = scenario(
        R"({"version":1,"output":{"serve":)" + std::to_string(port) + "}}",
        App::FuseOrbbec);
    ASSERT_TRUE(parsed) << parsed.status().message();
    EXPECT_EQ(parsed->output.serve, port);
  }
  for (const char* port : {"-1", "65536"}) {
    rejected(std::string(R"({"version":1,"output":{"serve":)") + port + "}}",
             App::FuseOrbbec, "output.serve: integer is out of range");
  }
  for (const char* port : {"1.5", "47801.0", R"("47801")", "true", "null"}) {
    rejected(std::string(R"({"version":1,"output":{"serve":)") + port + "}}",
             App::FuseOrbbec, "output.serve: expected an integer");
  }

  // A port alone codes, as a file does, so it carries a codec section; the
  // live scenario serves beside its stream.
  const auto served = scenario(
      R"({"version":1,"codec":{"intra_period":30},"output":{"serve":47801}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(served) << served.status().message();
  EXPECT_EQ(served->output.serve, 47801U);
  EXPECT_EQ(served->codec.intra_period, 30U);
  EXPECT_TRUE(served->output.csv.empty());
  EXPECT_TRUE(served->output.stream.empty());
  EXPECT_EQ(served->output.path, "/scenario/fuse_orbbec.ply");
  const auto recorded = scenario(
      R"({"version":1,"codec":{"intra_period":30},"output":{"path":"rig_end.ply","stream":"rig.vrst","serve":47801}})",
      App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(recorded) << recorded.status().message();
  EXPECT_EQ(recorded->output.serve, 47801U);
  EXPECT_EQ(recorded->output.stream, "/scenario/rig.vrst");
  // Streaming refuses the output section, a port too.
  for (const char* port : {"0", "47801"}) {
    rejected(
        std::string(R"({"version":1,"operation":"stream","output":{"serve":)") +
            port + "}}",
        App::FuseOrbbec, "output: is not used by operation stream");
  }

  // Every other profile refuses it, even at its default; those without
  // outputs refuse the output section.
  for (const auto app : every_profile()) {
    if (app == App::FuseOrbbec) continue;
    const bool outputs = app == App::FuseReplica || app == App::CodecReplica ||
                         app == App::CodecMesh || app == App::FuseRender ||
                         app == App::GfxModel || app == App::CalibRig ||
                         app == App::CalibColor || app == App::FuseRealsense;
    for (const char* port : {"0", "47801"}) {
      rejected(with_sections(
                   app, std::string(R"(,"output":{"serve":)") + port + "}"),
               app,
               (outputs ? "output.serve" : "output") +
                   std::string(": is not used by this application"));
    }
  }
}

TEST(Config, FuseOrbbecLimitsTheViewersItServes) {
  // 4 by default, served or not.
  EXPECT_EQ(Output{}.max_viewers, 4U);
  const auto plain = scenario(R"({"version":1})", App::FuseOrbbec);
  ASSERT_TRUE(plain) << plain.status().message();
  EXPECT_EQ(plain->output.max_viewers, 4U);
  const auto served = scenario(
      R"({"version":1,"codec":{"intra_period":30},"output":{"serve":47801}})",
      App::FuseOrbbec);
  ASSERT_TRUE(served) << served.status().message();
  EXPECT_EQ(served->output.max_viewers, 4U);
  for (const unsigned viewers : {1U, 4U, 64U, 4294967295U}) {
    const auto parsed = scenario(
        R"({"version":1,"codec":{"intra_period":30},"output":{"serve":47801,"max_viewers":)" +
            std::to_string(viewers) + "}}",
        App::FuseOrbbec);
    ASSERT_TRUE(parsed) << parsed.status().message();
    EXPECT_EQ(parsed->output.serve, 47801U);
    EXPECT_EQ(parsed->output.max_viewers, viewers);
  }
  const std::string serving =
      R"({"version":1,"codec":{"intra_period":30},"output":{"serve":47801,"max_viewers":)";
  rejected(serving + "0}}", App::FuseOrbbec,
           "output.max_viewers: must be finite and positive");
  for (const char* viewers : {"-1", "4294967296"}) {
    rejected(serving + viewers + "}}", App::FuseOrbbec,
             "output.max_viewers: integer is out of range");
  }
  for (const char* viewers : {"1.5", "4.0", R"("4")", "true", "null"}) {
    rejected(serving + viewers + "}}", App::FuseOrbbec,
             "output.max_viewers: expected an integer");
  }

  // Only beside a port, even at its default or at a count it would refuse;
  // streaming refuses the output section.
  for (const char* viewers : {"0", "4", "8"}) {
    rejected(std::string(R"({"version":1,"output":{"max_viewers":)") + viewers +
                 "}}",
             App::FuseOrbbec,
             "output.max_viewers: is not used when output.serve is 0");
    rejected(std::string(R"({"version":1,"output":{"serve":0,"max_viewers":)") +
                 viewers + "}}",
             App::FuseOrbbec,
             "output.max_viewers: is not used when output.serve is 0");
    rejected(
        std::string(
            R"({"version":1,"operation":"stream","output":{"max_viewers":)") +
            viewers + "}}",
        App::FuseOrbbec, "output: is not used by operation stream");
  }

  // Every other profile refuses it; those without outputs refuse the output
  // section.
  for (const auto app : every_profile()) {
    if (app == App::FuseOrbbec) continue;
    const bool outputs = app == App::FuseReplica || app == App::CodecReplica ||
                         app == App::CodecMesh || app == App::FuseRender ||
                         app == App::GfxModel || app == App::CalibRig ||
                         app == App::CalibColor || app == App::FuseRealsense;
    rejected(with_sections(app, R"(,"output":{"max_viewers":4})"), app,
             (outputs ? "output.max_viewers" : "output") +
                 std::string(": is not used by this application"));
  }
}

TEST(Config, PlayerPlaysAStreamFileOrALiveSender) {
  // A stream file, against the scenario's directory, in a 1280x720 window
  // without validation, until the window closes.
  const auto file =
      scenario(R"({"version":1,"source":{"path":"../streams/./rafa2.vrst"}})",
               App::Player, "/scenario/config");
  ASSERT_TRUE(file) << file.status().message();
  EXPECT_EQ(file->operation, Operation::View);
  EXPECT_EQ(file->source.kind, SourceKind::Stream);
  EXPECT_EQ(file->source.path, "/scenario/streams/rafa2.vrst");
  EXPECT_TRUE(file->source.host.empty());
  EXPECT_EQ(file->source.port, 0U);
  EXPECT_EQ(file->window.width, 1280);
  EXPECT_EQ(file->window.height, 720);
  EXPECT_FALSE(file->window.validation);
  EXPECT_EQ(file->run.frames, -1);
  const auto absolute =
      scenario(R"({"version":1,"source":{"path":"/data/rig.vrst"}})",
               App::Player, "/scenario");
  ASSERT_TRUE(absolute) << absolute.status().message();
  EXPECT_EQ(absolute->source.path, "/data/rig.vrst");

  // A live sender, on the sender's default port, 0, unless one is named.
  EXPECT_TRUE(Source{}.host.empty());
  EXPECT_EQ(Source{}.port, 0U);
  for (const char* host : {"192.168.21.217", "lab"}) {
    const auto live = scenario(
        std::string(R"({"version":1,"source":{"host":")") + host + R"("}})",
        App::Player, "/scenario");
    ASSERT_TRUE(live) << live.status().message();
    EXPECT_EQ(live->source.host, host);
    EXPECT_EQ(live->source.port, 0U);
    EXPECT_TRUE(live->source.path.empty());
  }
  for (const unsigned port : {0U, 1U, 5000U, 65535U}) {
    const auto live =
        scenario(R"({"version":1,"source":{"host":"lab","port":)" +
                     std::to_string(port) + "}}",
                 App::Player);
    ASSERT_TRUE(live) << live.status().message();
    EXPECT_EQ(live->source.host, "lab");
    EXPECT_EQ(live->source.port, port);
  }

  // The window, and the CI smoke run's count of presents, as stated.
  const auto stated = scenario(
      R"({"version":1,"operation":"view","source":{"kind":"stream","host":"127.0.0.1","port":5000},"window":{"width":1600,"height":900,"validation":true},"run":{"frames":3}})",
      App::Player);
  ASSERT_TRUE(stated) << stated.status().message();
  EXPECT_EQ(stated->source.host, "127.0.0.1");
  EXPECT_EQ(stated->source.port, 5000U);
  EXPECT_EQ(stated->window.width, 1600);
  EXPECT_EQ(stated->window.height, 900);
  EXPECT_TRUE(stated->window.validation);
  EXPECT_EQ(stated->run.frames, 3);
}

TEST(Config, PlayerNeedsOneStreamAndRefusesWhatItDoesNotUse) {
  const auto refused = [](const std::string& text, const std::string& reason) {
    rejected(text, App::Player, reason);
  };
  // A Player scenario whose source has these fields.
  const auto source = [](const std::string& fields) {
    return R"({"version":1,"source":{"kind":"stream",)" + fields + "}}";
  };

  // Exactly one stream: a file or a sender.
  refused(R"({"version":1})", "source: is required");
  refused(R"({"version":1,"source":{"kind":"stream"}})",
          "source: path or host is required");
  for (const char* both : {R"("path":"rig.vrst","host":"lab")",
                           R"("host":"lab","path":"rig.vrst")",
                           R"("path":"rig.vrst","host":"lab","port":5000)"}) {
    refused(source(both), "source: path and host are mutually exclusive");
  }
  for (const char* key : {"path", "host"}) {
    const std::string field = std::string("source.") + key;
    const std::string member = std::string("\"") + key + "\":";
    refused(source(member + R"("")"), field + ": must not be empty");
    refused(source(member + R"("a\u0000b")"), field + ": embedded NUL");
    refused(source(member + "1"), field + ": expected a string");
  }
  // The port is a sender's, in a field of its own; the client speaks IPv4.
  for (const char* port : {"0", "5000"}) {
    refused(source(std::string(R"("path":"rig.vrst","port":)") + port),
            "source.port: requires source.host");
  }
  refused(source(R"("port":5000)"), "source: path or host is required");
  for (const char* host : {"lab:5000", "lab:", "::1", "[::1]:5000"}) {
    refused(source(std::string(R"("host":")") + host + "\""),
            "source.host: must not contain ':'; the port is source.port");
  }
  for (const char* port : {"-1", "65536"}) {
    refused(source(std::string(R"("host":"lab","port":)") + port),
            "source.port: integer is out of range");
  }
  for (const char* port : {"1.0", "1.5", R"("5000")", "true", "null"}) {
    refused(source(std::string(R"("host":"lab","port":)") + port),
            "source.port: expected an integer");
  }

  // A stream source, and it views it.
  for (const char* kind : {"none", "dataset", "live", "mesh", "model"}) {
    refused(std::string(R"({"version":1,"source":{"kind":")") + kind +
                R"(","path":"rig.vrst"}})",
            "source.kind: is not supported by this application");
  }
  for (const char* operation :
       {"fuse", "stream", "codec", "project", "calibrate"}) {
    refused(std::string(R"({"version":1,"operation":")") + operation +
                R"(","source":{"path":"rig.vrst"}})",
            "operation: is not supported by this application");
  }
  // Source fields of the other profiles.
  for (const char* field :
       {R"("format":"replica")", R"("camera_parameters":"cam.json")",
        R"("preload":false)", R"("driver":"orbbec")",
        R"("serial":"CL2A141000N")", R"("sync_config":"rig.json")",
        R"("calibration":"poses.json")", R"("apply_sync":false)",
        R"("host_clock":false)", R"("color_codec":"mjpeg")",
        R"("color_width":1280)", R"("fps":30)", R"("height_m":1.7)",
        R"("metres_per_unit":1)", R"("up_axis":"y")", R"("up_vector":[0,1,0])",
        R"("sdf_mode":"signed")", R"("shell_voxels":1.5)",
        R"("allow_holes":false)"}) {
    const std::string_view text(field);
    const std::string key(text.substr(1, text.find('"', 1) - 1));
    refused(source(std::string(R"("path":"rig.vrst",)") + field),
            "source." + key + ": is not used by this application");
  }
  // Sections it has no use for, even empty.
  for (const char* section :
       {"fusion", "codec", "render", "output", "calibration", "diagnostics"}) {
    refused(std::string(R"({"version":1,"source":{"path":"rig.vrst"},")") +
                section + R"(":{}})",
            std::string(section) + ": is not used by this application");
  }
  // Its panel is its own.
  for (const char* overlay : {"true", "false"}) {
    refused(
        std::string(
            R"({"version":1,"source":{"path":"rig.vrst"},"window":{"overlay":)") +
            overlay + "}}",
        "window.overlay: is not used by this application");
  }
  refused(
      R"({"version":1,"source":{"path":"rig.vrst"},"window":{"width":0,"height":720}})",
      "window.width: must be finite and positive");
  refused(
      R"({"version":1,"source":{"path":"rig.vrst"},"window":{"height":-1}})",
      "window.height: must be finite and positive");
  // A count of presents, when given, is positive.
  for (const char* frames : {"0", "-1"}) {
    refused(
        std::string(
            R"({"version":1,"source":{"path":"rig.vrst"},"run":{"frames":)") +
            frames + "}}",
        "run.frames: must be positive");
  }
  for (const char* key : {"sets", "first"}) {
    refused(
        std::string(R"({"version":1,"source":{"path":"rig.vrst"},"run":{")") +
            key + R"(":0}})",
        std::string("run.") + key + ": is not used by this application");
  }

  // Every other profile refuses a stream source, and its host and port.
  for (const auto app : every_profile()) {
    if (app == App::Player) continue;
    rejected(R"({"version":1,"source":{"kind":"stream"}})", app,
             "source.kind: is not supported by this application");
    for (const char* field :
         {R"("host":"lab")", R"("port":0)", R"("port":5000)"}) {
      const std::string_view text(field);
      const std::string key(text.substr(1, text.find('"', 1) - 1));
      rejected(std::string(R"({"version":1,"source":{)") + field + "}}", app,
               "source." + key + ": is not used by this application");
    }
  }
}

TEST(Config, OnlyCalibColorSetsTheColourCamerasExposureAndGain) {
  // Every other profile refuses both, even at 0.
  for (const auto app : every_profile()) {
    if (app == App::CalibColor) continue;
    for (const char* field : {R"("color_exposure":200)", R"("color_gain":32)",
                              R"("color_exposure":0)", R"("color_gain":0)"}) {
      const std::string_view text(field);
      const std::string key(text.substr(1, text.find('"', 1) - 1));
      rejected(std::string(R"({"version":1,"source":{)") + field + "}}", app,
               "source." + key + ": is not used by this application");
    }
  }
}

TEST(Config, NoOutputOverwritesTheRigFilesARunReads) {
  // Each of FuseOrbbec's outputs against its sync file and calibration, in two
  // spellings of one path.
  for (const char* output : {"path", "csv", "stream"}) {
    for (const char* source : {"sync_config", "calibration"}) {
      for (const char* spelling : {"./rig.json", "/scenario/rig.json"}) {
        rejected(std::string(R"({"version":1,"source":{")") + source +
                     R"(":"rig.json"},"output":{")" + output + R"(":")" +
                     spelling + R"("}})",
                 App::FuseOrbbec,
                 std::string("output.") + output +
                     ": must differ from source." + source);
      }
    }
  }
  // CalibRig's session root is not its sync file.
  rejected(
      R"({"version":1,"source":{"sync_config":"rig.json"},"output":{"path":"./rig.json"}})",
      App::CalibRig, "output.path: must differ from source.sync_config");
}

TEST(Config, OnlyTheLiveFusingProfilesDefaultToDynamic) {
  for (const auto app : every_profile()) {
    const bool calibrates = app == App::CalibRig || app == App::CalibColor;
    const auto parsed = scenario(
        with_sections(app, calibrates ? R"(,"output":{"path":"s"})" : ""), app);
    ASSERT_TRUE(parsed) << parsed.status().message();
    EXPECT_EQ(parsed->fusion.dynamic, app == App::FuseOrbbec ||
                                          app == App::FuseRealsense ||
                                          app == App::RigViewer)
        << static_cast<int>(app);
  }
}

TEST(Config, OnlyTheLiveFusingProfilesFreeStaleBlocks) {
  // FuseOrbbec and FuseRealsense take RigViewer's key: every 30 sets by
  // default, 0 for never.
  EXPECT_EQ(Fusion{}.free_after, 30);
  for (const auto app : {App::FuseOrbbec, App::FuseRealsense, App::RigViewer}) {
    const auto omitted = scenario(with_sections(app, ""), app);
    ASSERT_TRUE(omitted) << omitted.status().message();
    EXPECT_EQ(omitted->fusion.free_after, 30);
    for (const int free_after : {0, 1, 30, 1000}) {
      const auto parsed =
          scenario(with_sections(app, R"(,"fusion":{"free_after":)" +
                                          std::to_string(free_after) + "}"),
                   app);
      ASSERT_TRUE(parsed) << parsed.status().message();
      EXPECT_EQ(parsed->fusion.free_after, free_after);
    }
    for (const auto& [value, reason] :
         {std::pair{"-1", "must be finite and nonnegative"},
          std::pair{"1.5", "expected an integer"},
          std::pair{"2147483648", "integer is out of range"}}) {
      rejected(with_sections(app, std::string(R"(,"fusion":{"free_after":)") +
                                      value + "}"),
               app, std::string("fusion.free_after: ") + reason);
    }
  }
  // A rig stream fused as rig_viewer fuses it.
  const auto dynamic = scenario(
      R"({"version":1,"fusion":{"dynamic":true,"free_after":10},"output":{"stream":"rig.vrst"}})",
      App::FuseOrbbec);
  ASSERT_TRUE(dynamic) << dynamic.status().message();
  EXPECT_TRUE(dynamic->fusion.dynamic);
  EXPECT_EQ(dynamic->fusion.free_after, 10);
  rejected(R"({"version":1,"operation":"stream","fusion":{"free_after":30}})",
           App::FuseOrbbec, "fusion: is not used by operation stream");
  // RigViewer's other fusion keys stay its own.
  rejected(R"({"version":1,"fusion":{"hold_ms":2000}})", App::FuseOrbbec,
           "fusion.hold_ms: is not used by this application");
  rejected(R"({"version":1,"fusion":{"depth_within_color":true}})",
           App::FuseOrbbec,
           "fusion.depth_within_color: is not used by this application");

  // Every other profile refuses it, even at its default; those that do not
  // fuse refuse the fusion section.
  for (const auto app : every_profile()) {
    if (app == App::FuseOrbbec || app == App::FuseRealsense ||
        app == App::RigViewer)
      continue;
    const bool fuses = app != App::GfxTriangle && app != App::GfxImGui &&
                       app != App::GfxModel && app != App::GfxImage &&
                       app != App::CalibProjectPoint && app != App::CalibRig &&
                       app != App::Player && app != App::CalibColor;
    rejected(with_sections(app, R"(,"fusion":{"free_after":30})"), app,
             (fuses ? "fusion.free_after" : "fusion") +
                 std::string(": is not used by this application"));
  }
}

TEST(Config, FusionAccumulateIsAnUnknownField) {
  // 0.13.0's per-set clear is gone: every profile refuses the key, whatever
  // its value, before it asks whether the profile fuses.
  for (const auto app : every_profile()) {
    for (const char* value : {"true", "false"}) {
      rejected(with_sections(app, std::string(R"(,"fusion":{"accumulate":)") +
                                      value + "}"),
               app, "fusion.accumulate: unknown field");
    }
  }
}

TEST(Config, MeshSourceRequiresOneScaleAndOneNonzeroUpDirection) {
  const auto parsed = scenario(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"-y"}})",
      App::CodecMesh, "/scenario");
  ASSERT_TRUE(parsed) << parsed.status().message();
  ASSERT_TRUE(parsed->source.up_vector);
  const auto up = parsed->source.up_vector;
  if (up.has_value())
    EXPECT_EQ(up.value()[1], -1.);
  else
    FAIL() << "up_axis was not resolved to a vector";
  const auto custom_voxel = scenario(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"},"fusion":{"voxel_size_m":0.02}})",
      App::CodecMesh);
  ASSERT_TRUE(custom_voxel) << custom_voxel.status().message();
  EXPECT_EQ(custom_voxel->fusion.truncation_m, 0.08F);
  rejected(R"({"version":1,"source":{"path":"mesh.ply","height_m":2}})",
           App::CodecMesh, "up_vector");
  rejected(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_vector":[0,0,0]}})",
      App::CodecMesh, "up_vector");
  rejected(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y","up_vector":[1,0,0]}})",
      App::CodecMesh, "mutually exclusive");
  rejected(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"bad"}})",
      App::CodecMesh, "up_axis");

  // The scale is a declared unit or a height to fit: exactly one of the two.
  const auto unit = scenario(
      R"({"version":1,"source":{"path":"mesh.ply","metres_per_unit":0.001,"up_axis":"z"}})",
      App::CodecMesh);
  ASSERT_TRUE(unit) << unit.status().message();
  EXPECT_EQ(unit->source.metres_per_unit, 0.001);
  EXPECT_EQ(unit->source.height_m, 0.);
  // Inspection takes either, as it reads the mesh at its scale.
  for (const char* scale : {R"("height_m":1.7)", R"("metres_per_unit":2.5)"}) {
    const auto inspected = scenario(
        std::string(R"({"version":1,"source":{"path":"mesh.ply",)") + scale +
            R"(,"up_axis":"y"},"codec":{"inspect_only":true}})",
        App::CodecMesh);
    ASSERT_TRUE(inspected) << inspected.status().message();
  }
  // A mesh source with these scale fields, each followed by a comma.
  const auto scaled = [](const std::string& fields) {
    return R"({"version":1,"source":{"path":"mesh.ply",)" + fields +
           R"("up_axis":"y"}})";
  };
  // A mesh source with one scale field.
  const auto one = [&scaled](const char* key, const char* value) {
    return scaled(R"(")" + std::string(key) + R"(":)" + value + ",");
  };
  rejected(scaled(""), App::CodecMesh,
           "source: height_m or metres_per_unit is required");
  for (const char* both : {R"("height_m":1.7,"metres_per_unit":1,)",
                           R"("metres_per_unit":1,"height_m":1.7,)",
                           R"("height_m":0,"metres_per_unit":1,)"}) {
    rejected(scaled(both), App::CodecMesh,
             "source: height_m and metres_per_unit are mutually exclusive");
  }
  for (const char* scale : {"height_m", "metres_per_unit"}) {
    const std::string field = std::string("source.") + scale;
    // 1e-999 rounds to zero.
    for (const char* value : {"0", "-1", "-0.5", "1e-999"}) {
      rejected(one(scale, value), App::CodecMesh,
               field + ": must be finite and positive");
    }
    rejected(one(scale, "1e999"), App::CodecMesh, "overflow");
    rejected(one(scale, R"("1")"), App::CodecMesh,
             field + ": expected a number");
  }
  rejected(R"({"version":1,"source":{"path":"scene","metres_per_unit":1}})",
           App::CodecReplica,
           "source.metres_per_unit: is not used by this application");
}

TEST(Config, MeshSequenceRangeIsGenericAndThePatternIsTheConsumers) {
  // A CodecMesh scenario with these source fields and this run section.
  const auto mesh = [](const std::string& path, const std::string& run) {
    return R"({"version":1,"source":{"path":")" + path +
           R"(","height_m":1.7,"up_axis":"y"},"run":)" + run + "}";
  };
  // Core resolves the path and leaves its frame-number pattern alone.
  const auto sequence =
      scenario(mesh("Frame_%05d_textured.obj", R"({"first":1,"frames":150})"),
               App::CodecMesh, "/scenario");
  ASSERT_TRUE(sequence) << sequence.status().message();
  EXPECT_EQ(sequence->source.path, "/scenario/Frame_%05d_textured.obj");
  EXPECT_EQ(sequence->run.first, 1);
  EXPECT_EQ(sequence->run.frames, 150);
  // A count without a pattern loads; the consumer refuses it at runtime.
  const auto plain = scenario(mesh("mesh.ply", R"({"frames":5})"),
                              App::CodecMesh, "/scenario");
  ASSERT_TRUE(plain) << plain.status().message();
  EXPECT_EQ(plain->run.first, 0);
  EXPECT_EQ(plain->run.frames, 5);
  const auto from = scenario(mesh("f%d.obj", R"({"first":7})"), App::CodecMesh);
  ASSERT_TRUE(from) << from.status().message();
  EXPECT_EQ(from->run.first, 7);
  EXPECT_EQ(from->run.frames, 1);
  // The last frame number may be INT32_MAX itself.
  const auto last = scenario(
      mesh("f%d.obj", R"({"first":2147483600,"frames":48})"), App::CodecMesh);
  ASSERT_TRUE(last) << last.status().message();
  EXPECT_EQ(last->run.first + (last->run.frames - 1), 2147483647);

  // frames is an exact count, so it is at least one.
  for (const char* frames : {"0", "-1"}) {
    rejected(mesh("f%d.obj", std::string(R"({"frames":)") + frames + "}"),
             App::CodecMesh, "run.frames: must be positive");
  }
  rejected(mesh("f%d.obj", R"({"first":-1})"), App::CodecMesh,
           "run.first: must be finite and nonnegative");
  rejected(mesh("f%d.obj", R"({"first":1.5})"), App::CodecMesh,
           "run.first: expected an integer");
  rejected(mesh("f%d.obj", R"({"first":2147483648})"), App::CodecMesh,
           "run.first: integer is out of range");
  for (const char* run : {R"({"first":2147483600,"frames":100})",
                          R"({"first":2147483600,"frames":49})",
                          R"({"first":2147483647,"frames":2})"}) {
    rejected(mesh("f%d.obj", run), App::CodecMesh,
             "run.frames: run.first + run.frames - 1 must not exceed "
             "INT32_MAX");
  }
  rejected(mesh("f%d.obj", R"({"sets":1})"), App::CodecMesh,
           "run.sets: is not used by this application");
  // Only CodecMesh numbers its input.
  for (const auto app : {App::CodecReplica, App::FuseReplica}) {
    rejected(R"({"version":1,"source":{"path":"scene"},"run":{"first":0}})",
             app, "run.first: is not used by this application");
  }
  rejected(R"({"version":1,"run":{"first":1}})", App::FuseOrbbec,
           "run.first: is not used by this application");
  rejected(
      R"({"version":1,"source":{"sync_config":"sync.json"},"run":{"first":1}})",
      App::RigViewer, "run.first: is not used by this application");

  // Inspection reads the whole range.
  const auto inspected = scenario(
      R"({"version":1,"source":{"path":"f%d.obj","height_m":1.7,"up_axis":"y"},"codec":{"inspect_only":true},"run":{"first":1,"frames":3}})",
      App::CodecMesh);
  ASSERT_TRUE(inspected) << inspected.status().message();
  EXPECT_EQ(inspected->run.frames, 3);
}

TEST(Config, OnlyTheMeshPathTheUserWroteCarriesAPattern) {
  // CodecMesh's source.path, resolved against this base directory.
  const auto mesh_path = [](const std::string& path, const char* base) {
    const auto parsed = scenario(
        R"({"version":1,"source":{"path":")" + path +
            R"(","height_m":1.7,"up_axis":"y"},"output":{"csv":"rows.csv"}})",
        App::CodecMesh, base);
    EXPECT_TRUE(parsed) << parsed.status().message();
    if (!parsed) return std::string();
    EXPECT_EQ(parsed->output.csv, std::string(base) + "/rows.csv");
    return parsed->source.path;
  };
  // A '%' in the scenario's directory is text; the user's own is pattern.
  EXPECT_EQ(mesh_path("cube.obj", "/pct/50%off"), "/pct/50%%off/cube.obj");
  EXPECT_EQ(mesh_path("mesh.obj", "/pct/take%d"), "/pct/take%%d/mesh.obj");
  EXPECT_EQ(mesh_path("f%05d.obj", "/pct/take%d"), "/pct/take%%d/f%05d.obj");
  EXPECT_EQ(mesh_path("a%%b.obj", "/pct"), "/pct/a%%b.obj");
  // A ../ removes the escaped directories it leaves.
  EXPECT_EQ(mesh_path("../cube.obj", "/pct/50%off"), "/pct/cube.obj");
  EXPECT_EQ(mesh_path("../b/f%d.obj", "/1%/a%b"), "/1%%/b/f%d.obj");
  // An absolute path is all the user's.
  EXPECT_EQ(mesh_path("/data/50%off/f%05d.obj", "/pct/take%d"),
            "/data/50%off/f%05d.obj");
  // No other profile's path is a pattern.
  for (const auto app : {App::FuseReplica, App::CodecReplica}) {
    const auto replica = scenario(R"({"version":1,"source":{"path":"scene"}})",
                                  app, "/pct/50%off");
    ASSERT_TRUE(replica) << replica.status().message();
    EXPECT_EQ(replica->source.path, "/pct/50%off/scene");
    EXPECT_EQ(replica->source.camera_parameters, "/pct/50%off/cam_params.json");
  }
  const auto model = scenario(R"({"version":1,"source":{"path":"cube.glb"}})",
                              App::GfxModel, "/pct/take%d");
  ASSERT_TRUE(model) << model.status().message();
  EXPECT_EQ(model->source.path, "/pct/take%d/cube.glb");
}

TEST(Config, CodecMeshTruncationAndHoles) {
  const std::string source =
      R"({"version":1,"source":{"path":"mesh.ply","height_m":1.7,"up_axis":"y")";
  // The paper's band: eight 5 mm voxels.
  const auto band = scenario(
      source + R"(},"fusion":{"voxel_size_m":0.005,"truncation_m":0.04}})",
      App::CodecMesh);
  ASSERT_TRUE(band) << band.status().message();
  EXPECT_EQ(band->fusion.truncation_m, 0.04F);
  for (const char* trunc : {"0", "-1"}) {
    const auto sentinel = scenario(
        source + R"(},"fusion":{"voxel_size_m":0.005,"truncation_m":)" + trunc +
            "}}",
        App::CodecMesh);
    ASSERT_TRUE(sentinel) << sentinel.status().message();
    EXPECT_EQ(sentinel->fusion.truncation_m, 4.F * 0.005F);
  }
  // 127 blocks of eight 5 mm voxels is 5.08 m.
  const auto wide = scenario(
      source + R"(},"fusion":{"voxel_size_m":0.005,"truncation_m":5}})",
      App::CodecMesh);
  ASSERT_TRUE(wide) << wide.status().message();
  EXPECT_EQ(wide->fusion.truncation_m, 5.F);
  rejected(source + R"(},"fusion":{"voxel_size_m":0.005,"truncation_m":5.1}})",
           App::CodecMesh,
           "fusion.truncation_m: must be at most 127 blocks of eight voxels");
  // Inspection converts nothing, so it refuses the whole section.
  rejected(
      source +
          R"(},"codec":{"inspect_only":true},"fusion":{"truncation_m":0.04}})",
      App::CodecMesh, "fusion: is not used when codec.inspect_only is enabled");
  // Truncation alone joins CodecMesh: a mesh has no depth limits or weights,
  // and the live profiles that do keep them.
  for (const char* key : {"min_depth_m", "max_depth_m", "max_weight"}) {
    const std::string field = std::string("fusion.") + key;
    rejected(source + R"(},"fusion":{")" + key + R"(":1}})", App::CodecMesh,
             field + ": is not used by this application");
    const auto live = scenario(
        std::string(
            R"({"version":1,"source":{"sync_config":"sync.json"},"fusion":{")") +
            key + R"(":1}})",
        App::RigViewer);
    ASSERT_TRUE(live) << live.status().message();
  }
  const auto rig = scenario(
      R"({"version":1,"source":{"sync_config":"sync.json"},"fusion":{"truncation_m":0.05}})",
      App::RigViewer);
  ASSERT_TRUE(rig) << rig.status().message();
  EXPECT_EQ(rig->fusion.truncation_m, 0.05F);

  // Holes: signed mode only, and only when asked.
  for (const char* mode : {"", R"(,"sdf_mode":"signed")"}) {
    for (const char* allow : {"true", "false"}) {
      const auto holes = scenario(
          source + mode + R"(,"allow_holes":)" + allow + "}}", App::CodecMesh);
      ASSERT_TRUE(holes) << holes.status().message();
      EXPECT_EQ(holes->source.allow_holes, std::string_view(allow) == "true");
    }
  }
  for (const char* allow : {"true", "false"}) {
    rejected(source + R"(,"sdf_mode":"shell","allow_holes":)" + allow + "}}",
             App::CodecMesh,
             "source.allow_holes: is not used by source.sdf_mode shell");
    rejected(source + R"(,"allow_holes":)" + allow +
                 R"(},"codec":{"inspect_only":true}})",
             App::CodecMesh,
             "source.allow_holes: is not used when codec.inspect_only is "
             "enabled");
    rejected(
        std::string(R"({"version":1,"source":{"path":"scene","allow_holes":)") +
            allow + "}}",
        App::CodecReplica,
        "source.allow_holes: is not used by this application");
  }
  rejected(source + R"(,"allow_holes":1}})", App::CodecMesh,
           "source.allow_holes: expected a boolean");

  // The shell's bound against voxel and truncation is the conversion's.
  for (const char* shell : {"0.5", "5"}) {
    const auto loaded = scenario(
        source + R"(,"sdf_mode":"shell","shell_voxels":)" + shell + "}}",
        App::CodecMesh);
    ASSERT_TRUE(loaded) << loaded.status().message();
  }
  for (const char* shell : {"0", "-1"}) {
    rejected(source + R"(,"sdf_mode":"shell","shell_voxels":)" + shell + "}}",
             App::CodecMesh,
             "source.shell_voxels: must be finite and positive");
  }
}

TEST(Config, KnownButUnusedSettingsAreRejectedRatherThanSilentlyIgnored) {
  rejected(
      R"({"version":1,"output":{"screenshot":"a.png"},"window":{"overlay":true}})",
      App::GfxModel, "window.overlay");
  rejected(R"({"version":1,"fusion":{}})", App::GfxImage, "fusion");
  rejected(R"({"version":1,"render":{"texture":false}})", App::GfxImage,
           "render");
  rejected(R"({"version":1,"window":{"overlay":false}})", App::GfxTriangle,
           "window.overlay");
  rejected(R"({"version":1,"source":{"serial":"A"}})", App::GfxModel,
           "source.serial");
  rejected(R"({"version":1,"run":{"sets":1}})", App::GfxModel, "run.sets");
  rejected(R"({"version":1,"run":{"first":0}})", App::GfxModel, "run.first");
  rejected(
      R"({"version":1,"output":{"screenshot":"a.png"},"run":{"frames":1}})",
      App::GfxModel, "run.frames");
  rejected(R"({"version":1,"window":{"width":100}})", App::Scanner, "window");
  rejected(R"({"version":1,"fusion":{"max_weight":5}})", App::Scanner,
           "fusion.max_weight");
  rejected(R"({"version":1,"run":{}})", App::Scanner, "run");
}

TEST(Config, ScannerAndProjectionStaticPoliciesAreCheckedWithoutHardware) {
  const auto scanner = scenario(
      R"({"version":1,"fusion":{"voxel_size_m":0.02,"truncation_m":0.06,"buckets":2048,"max_buckets":4096,"occlusion_m":0},"render":{"texture":false},"diagnostics":{"measure_stages":false}})",
      App::Scanner);
  ASSERT_TRUE(scanner) << scanner.status().message();
  EXPECT_FALSE(scanner->render.texture);
  EXPECT_FALSE(scanner->diagnostics.measure_stages);
  rejected(R"({"version":1,"fusion":{"truncation_m":0}})", App::Scanner,
           "fusion.truncation_m");
  rejected(R"({"version":1,"fusion":{"buckets":2048,"max_buckets":1024}})",
           App::Scanner, "fusion.max_buckets");
  rejected(R"({"version":1,"calibration":{"point":[0,0,0]}})",
           App::CalibProjectPoint, "calibration.point");
  rejected(R"({"version":1,"calibration":{"intrinsics":[0,100,50,50]}})",
           App::CalibProjectPoint, "calibration.intrinsics");
}

TEST(Config, EveryPathIsRelativeToTheConfigurationDirectory) {
  const auto model = scenario(
      R"({"version":1,"source":{"path":"models/../cube.glb"},"output":{"screenshot":"images/cube.png"}})",
      App::GfxModel, "/scenario");
  ASSERT_TRUE(model) << model.status().message();
  EXPECT_EQ(model->source.path, "/scenario/cube.glb");
  EXPECT_EQ(model->output.screenshot, "/scenario/images/cube.png");
  const auto data = scenario(
      R"({"version":1,"source":{"path":"dataset","camera_parameters":"camera.json"},"output":{"prefix":"runs/trial"}})",
      App::CodecReplica, "/scenario");
  ASSERT_TRUE(data) << data.status().message();
  EXPECT_EQ(data->source.camera_parameters, "/scenario/camera.json");
  EXPECT_EQ(data->output.prefix, "/scenario/runs/trial");
  rejected(R"({"version":1,"source":{"path":"a\u0000b"}})", App::GfxModel,
           "embedded NUL");
  rejected(R"({"version":1,"source":{"path":""}})", App::GfxModel, "empty");
}

TEST(Config, DiscriminatorAndOptionalOutputPolicyAreExplicit) {
  EXPECT_FALSE(
      parse(R"({"version":1,"operation":"view","source":{"path":"cube.glb"}})",
            App::GfxModel));
  EXPECT_FALSE(parse(R"({"version":1,"operation":"fuse"})", App::FuseOrbbec));
  const auto disabled =
      scenario(R"({"version":1,"output":{"path":""}})", App::FuseOrbbec);
  ASSERT_FALSE(disabled);
  EXPECT_NE(disabled.status().message().find("output.path"), std::string::npos);
  const auto optional =
      scenario(R"({"version":1,"output":{"screenshot":""}})", App::GfxModel);
  ASSERT_TRUE(optional) << optional.status().message();
  EXPECT_TRUE(optional->output.screenshot.empty());
  rejected(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"},"codec":{"inspect_only":true,"sweep":true}})",
      App::CodecMesh, "codec.sweep");
}

TEST(Config, InputSizeAndNestingAreBoundedBeforeDecoding) {
  const std::string at_limit = std::string(kMaxConfigDepth, '[') + "0" +
                               std::string(kMaxConfigDepth, ']');
  const auto bounded = parse(at_limit, App::GfxModel);
  ASSERT_FALSE(bounded);
  EXPECT_NE(bounded.status().message().find("expected an object"),
            std::string::npos);
  const auto too_deep = parse("[" + at_limit + "]", App::GfxModel);
  ASSERT_FALSE(too_deep);
  EXPECT_NE(too_deep.status().message().find("nesting"), std::string::npos);
  rejected(std::string(kMaxConfigBytes + 1, ' '), App::GfxImage, "one MiB");
  rejected(std::string(kMaxConfigDepth + 1, '[') + "0" +
               std::string(kMaxConfigDepth + 1, ']'),
           App::GfxImage, "nesting");
  // Brackets and overflow-like text inside strings are ordinary JSON text.
  const auto parsed = scenario(
      R"({"version":1,"source":{"path":"[[[1e999\\\".glb"}})", App::GfxModel);
  ASSERT_TRUE(parsed) << parsed.status().message();
}

TEST(Config, CommandLineOnlyAcceptsConfigurationOrHelpWithoutSideEffects) {
  const char* const args[] = {"app", "--config", "scenario.json"};
  auto parsed = parse_args(3, args);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->config_path, "scenario.json");
  EXPECT_FALSE(parsed->help);
  const char* const help[] = {"app", "--help"};
  parsed = parse_args(2, help);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->help);
  const char* const old[] = {"app", "--frames", "1"};
  EXPECT_FALSE(parse_args(3, old));
  const char* const duplicate[] = {"app", "--config", "a", "--config", "b"};
  EXPECT_FALSE(parse_args(5, duplicate));
  const char* const missing[] = {"app", "--config"};
  EXPECT_FALSE(parse_args(2, missing));
  EXPECT_FALSE(parse_args(1, args));
  EXPECT_FALSE(parse_args(1, nullptr));
  EXPECT_NE(std::string_view(usage()).find("--config"), std::string_view::npos);
}

TEST(Config, LoadBoundsRegularFilesAndUsesTheFileDirectory) {
  std::string name =
      (std::filesystem::temp_directory_path() / "vkc-config-XXXXXX").string();
  ASSERT_NE(mkdtemp(name.data()), nullptr);
  struct RemoveDirectory {
    std::filesystem::path path;
    ~RemoveDirectory() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };
  const RemoveDirectory cleanup{name};
  const std::string file = name + "/scenario.json";
  ASSERT_TRUE(write_file(
      file,
      R"({"version":1,"operation":"view","source":{"kind":"model","path":"cube.glb"}})",
      WriteMode::CreateNew));
  const auto result = load(file, App::GfxModel);
  ASSERT_TRUE(result) << result.status().message();
  EXPECT_EQ(result->source.path, name + "/cube.glb");
  EXPECT_EQ(load(name + "/missing.json", App::GfxModel).status().domain(),
            Status::Code::NotFound);
  EXPECT_EQ(load(name, App::GfxModel).status().domain(),
            Status::Code::InvalidArgument);
  const std::string fifo = name + "/fifo";
  ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_EQ(load(fifo, App::GfxModel).status().domain(),
            Status::Code::InvalidArgument);
  ASSERT_TRUE(write_file(file, std::string(kMaxConfigBytes + 1, ' '),
                         WriteMode::Replace));
  EXPECT_EQ(load(file, App::GfxModel).status().domain(),
            Status::Code::InvalidArgument);
}

}  // namespace
}  // namespace volumetric_kit::core::config
