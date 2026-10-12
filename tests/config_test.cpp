// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin
#include "volumetric_kit/core/config/config.hpp"

#include <array>
#include <cfenv>
#include <cstdlib>  // IWYU pragma: keep; glibc declares mkdtemp here.
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

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
        app == App::FuseRender || app == App::FuseViewer || app == App::Scanner)
      operation = "fuse";
    if (app == App::CodecReplica || app == App::CodecMesh) operation = "codec";
    if (app == App::CalibProjectPoint) operation = "project";
    if (text.find("\"operation\"") == std::string_view::npos)
      document.insert(1, std::string(R"("operation":")") + operation + "\"" +
                             (text.size() > 2 ? "," : ""));
    const char* kind = "none";
    if (app == App::FuseReplica || app == App::CodecReplica ||
        app == App::FuseRender || app == App::FuseViewer)
      kind = "dataset";
    if (app == App::FuseOrbbec || app == App::RigViewer || app == App::Scanner)
      kind = "live";
    if (app == App::CodecMesh) kind = "mesh";
    if (app == App::GfxModel) kind = "model";
    const auto source = document.find("\"source\":{");
    if (source != std::string::npos &&
        document.find("\"kind\"") == std::string::npos)
      document.insert(source + 10, std::string(R"("kind":")") + kind + "\",");
    if (document.find("\"source\"") == std::string::npos &&
        (app == App::FuseOrbbec || app == App::RigViewer ||
         app == App::Scanner))
      document.insert(1, std::string(R"("source":{"kind":")") + kind + "\"},");
  }
  return parse(document, app, base);
}

TEST(Config, OperationIsRequiredAndUnknownApplicationIsRejected) {
  const auto missing = parse(R"({"version":1})", App::GfxModel);
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.status().message().find("operation"), std::string::npos);
  const auto invalid_app = static_cast<App>(999);
  EXPECT_FALSE(defaults(invalid_app));
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
  const auto live = scenario(R"({"version":1})", App::FuseOrbbec, "/scenario");
  ASSERT_TRUE(live) << live.status().message();
  EXPECT_EQ(live->source.color_codec, ColorCodec::Hevc);
  EXPECT_EQ(live->source.color_width, 1280U);
  EXPECT_EQ(live->source.depth_width, 640U);
  EXPECT_EQ(live->source.depth_height, 576U);
  EXPECT_EQ(live->source.fps, 30U);
  EXPECT_EQ(live->run.frames, 300);
  EXPECT_EQ(live->fusion.min_depth_m, 0.25F);
  EXPECT_EQ(live->fusion.max_depth_m, 5.F);
  const auto rig =
      scenario(R"({"version":1,"source":{"sync_config":"sync.json"}})",
               App::RigViewer, "/scenario");
  ASSERT_TRUE(rig) << rig.status().message();
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
  rejected(R"({"version":1,"operation":"stream"})", App::GfxModel, "operation");
}

TEST(Config, LiveRelationsAndCountsAreCentral) {
  rejected(R"({"version":1,"source":{"serial":"A","sync_config":"rig.json"}})",
           App::FuseOrbbec, "source.serial");
  rejected(R"({"version":1,"source":{"apply_sync":true}})", App::FuseOrbbec,
           "source.apply_sync");
  rejected(
      R"({"version":1,"source":{"host_clock":true,"sync_config":"rig.json"}})",
      App::FuseOrbbec, "source.host_clock");
  rejected(R"({"version":1,"source":{"color_width":3840}})", App::FuseOrbbec,
           "color_height");
  rejected(R"({"version":1,"source":{"depth_width":640,"depth_height":0}})",
           App::FuseOrbbec, "source.depth_height");
  rejected(R"({"version":1,"source":{"fps":-1}})", App::FuseOrbbec,
           "source.fps");
  rejected(R"({"version":1,"source":{"fps":4294967296}})", App::FuseOrbbec,
           "source.fps");
  rejected(R"({"version":1,"run":{"frames":2147483648}})", App::FuseOrbbec,
           "run.frames");
  rejected(R"({"version":1,"run":{"frames":0}})", App::FuseOrbbec,
           "run.frames");
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

TEST(Config, DirectedRoundingCannotTurnOverflowIntoAValidFiniteSetting) {
  struct RoundingRestore {
    int mode = std::fegetround();
    ~RoundingRestore() { std::fesetround(mode); }
  };
  const RoundingRestore restore;
  for (const int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD}) {
    ASSERT_EQ(std::fesetround(mode), 0);
    for (const auto* const token :
         {"1e999", "-1e999", "1.25e999", "-1.25e999"}) {
      rejected(std::string(R"({"version":1,"calibration":{"point":[0,0,)") +
                   token + "]}}",
               App::CalibProjectPoint, "numeric overflow");
    }
    rejected(R"({"version":1,"fusion":{"voxel_size_m":3.5e38}})",
             App::FuseOrbbec, "fusion.voxel_size_m");
  }
}

TEST(Config, RepresentableDoubleAndUnderflowAreNotMisclassifiedAsOverflow) {
  const auto parsed = scenario(
      R"({"version":1,"calibration":{"distortion":[1e-999,0,0,0,0],"point":[1.7976931348623157e308,0,1]}})",
      App::CalibProjectPoint);
  ASSERT_TRUE(parsed) << parsed.status().message();
  EXPECT_EQ(parsed->calibration.distortion[0], 0.);
  EXPECT_EQ(parsed->calibration.point[0], std::numeric_limits<double>::max());
}

TEST(Config, CodecPresetWeightsAndRetainedStepBoundsAreResolvedOnce) {
  const auto parsed = scenario(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_table":"radial","coefficient_count":512,"quantization_scale":0.2}})",
      App::CodecReplica);
  ASSERT_TRUE(parsed) << parsed.status().message();
  EXPECT_EQ(parsed->codec.quantization_weights[0], 1.F);
  EXPECT_EQ(parsed->codec.quantization_weights[1 + (8 * 2) + (64 * 3)], 2.75F);
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"coefficient_count":513}})",
      App::CodecReplica, "coefficient_count");
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_scale":1e-40}})",
      App::CodecReplica, "quantization_scale");
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_scale":0.0001}})",
      App::CodecReplica, "quantization_scale");
  rejected(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_table":"radial","coefficient_count":512,"quantization_scale":4}})",
      App::CodecReplica, "quantization_scale");
  const auto dc_only = scenario(
      R"({"version":1,"source":{"path":"scene"},"codec":{"quantization_table":"radial","coefficient_count":1,"quantization_scale":64}})",
      App::CodecReplica);
  ASSERT_TRUE(dc_only) << dc_only.status().message();
}

TEST(Config, MeshSourceRequiresHeightAndOneNonzeroUpDirection) {
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
}

TEST(Config, TypedMutationsCannotInvalidateResolvedSourceChoices) {
  auto mesh = scenario(
      R"({"version":1,"source":{"path":"mesh.ply","height_m":2,"up_axis":"y"}})",
      App::CodecMesh);
  ASSERT_TRUE(mesh) << mesh.status().message();
  mesh->source.up_axis = "bad";
  auto status = validate(*mesh, App::CodecMesh);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message().find("source.up_axis"), std::string::npos);
  mesh->source.up_axis = "y";
  mesh->source.up_vector = std::array<double, 3>{1., 0., 0.};
  status = validate(*mesh, App::CodecMesh);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message().find("source.up_vector"), std::string::npos);
  mesh->source.up_vector = std::array<double, 3>{0., 1., 0.};
  EXPECT_TRUE(validate(*mesh, App::CodecMesh));
  auto live = defaults(App::FuseOrbbec);
  ASSERT_TRUE(live);
  live->source.serial = std::string("serial\0suffix", 13);
  status = validate(*live, App::FuseOrbbec);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message().find("source.serial"), std::string::npos);
}

TEST(Config, KnownButUnusedSettingsAreRejectedRatherThanSilentlyIgnored) {
  rejected(R"({"version":1,"fusion":{}})", App::GfxImage, "fusion");
  rejected(R"({"version":1,"render":{"texture":false}})", App::GfxImage,
           "render");
  rejected(R"({"version":1,"window":{"overlay":false}})", App::GfxTriangle,
           "window.overlay");
  rejected(R"({"version":1,"source":{"serial":"A"}})", App::GfxModel,
           "source.serial");
  rejected(R"({"version":1,"run":{"sets":1}})", App::GfxModel, "run.sets");
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
  rejected(std::string(kMaxConfigBytes + 1, ' '), App::GfxImage, "one MiB");
  rejected(std::string(kMaxConfigDepth + 1, '[') + "0" +
               std::string(kMaxConfigDepth + 1, ']'),
           App::GfxImage, "nesting");
  // Brackets and overflow-like text inside strings do not affect preflight.
  const auto parsed = scenario(
      R"({"version":1,"source":{"path":"[[[1e999\\\".glb"}})", App::GfxModel);
  ASSERT_TRUE(parsed) << parsed.status().message();
}

TEST(Config, TypedValidationAlsoRejectsIgnoredProgrammaticValues) {
  auto triangle = defaults(App::GfxTriangle);
  ASSERT_TRUE(triangle);
  triangle->source.path = "ignored.glb";
  EXPECT_FALSE(validate(*triangle, App::GfxTriangle));
  triangle->source.path.clear();
  triangle->window.overlay = false;
  EXPECT_FALSE(validate(*triangle, App::GfxTriangle));
  auto live = defaults(App::FuseOrbbec);
  ASSERT_TRUE(live);
  live->operation = Operation::Stream;
  EXPECT_FALSE(validate(*live, App::FuseOrbbec));
  live->output.path.clear();
  EXPECT_TRUE(validate(*live, App::FuseOrbbec));
  live->fusion.voxel_size_m = 0.04F;
  EXPECT_FALSE(validate(*live, App::FuseOrbbec));
  rejected(
      R"({"version":1,"output":{"screenshot":"a.png"},"window":{"overlay":true}})",
      App::GfxModel, "window.overlay");
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
