// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Links volumetric_kit::core and uses each piece of the base tier once -- and
// of the vulkan and camera tiers, when the core has them -- so a broken
// install, export or include path fails here rather than in a sibling. Nothing
// here needs a GPU.

#include <cstdio>
#include <string_view>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/version.hpp"

#ifdef VKC_CONSUMER_HAS_CAMERA
#include <vector>

#include "volumetric_kit/core/camera/lens.hpp"
#include "volumetric_kit/core/camera/rig_calibration.hpp"
#endif
#ifdef VKC_CONSUMER_HAS_VULKAN
#include "smoke_comp.spv.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#endif

namespace vkc = volumetric_kit::core;

// From the consumer's own library (parse.cpp).
vkc::Result<int> parse_positive(int x);

namespace {

vkc::Status run() {
  VKC_ASSIGN(const int n, parse_positive(3));
  VKC_CHECK(n == 3, "parsed what it was given");
  if (parse_positive(-1).ok()) return vkc::Status::numerical("accepted -1");
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
  // Embedded SPIR-V: whole words, opening with the SPIR-V magic number.
  VKC_CHECK(vkc_consumer_smoke_comp_spv_size % 4 == 0 &&
                vkc_consumer_smoke_comp_spv_size > 20 &&
                vkc_consumer_smoke_comp_spv[0] == 0x03 &&
                vkc_consumer_smoke_comp_spv[3] == 0x07,
            "embedded SPIR-V");
  std::printf("vulkan tier consumed\n");
#endif
#ifdef VKC_CONSUMER_HAS_CAMERA
  VKC_ASSIGN(const std::vector<vkc::RigCameraCalibration> rig,
             vkc::parse_rig_calibration(
                 R"({"device_calibration": {"A": {"pose": {"rvec": [0, 0, 0],
                     "tvec": [0, 0, 1]}}}})"));
  VKC_CHECK(rig.size() == 1 && rig[0].camera_to_world.translation.z == -1.0,
            "parsed the rig");
  VKC_ASSIGN(
      const vkc::PinholeIntrinsics half,
      vkc::scale_intrinsics({100.0, 100.0, 63.5, 31.5}, {128, 64}, {64, 32}));
  VKC_CHECK(half.cx == 31.5, "scaled about pixel centres");
  std::printf("camera tier consumed\n");
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
    std::fprintf(stderr, "package consumer failed: %s\n",
                 status.message().c_str());
    return 1;
  }
  std::printf("volumetric_kit_core %s consumed\n", vkc::version_string());
  return 0;
}
