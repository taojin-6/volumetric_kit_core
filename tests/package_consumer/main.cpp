// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Links volumetric_kit::core and uses each piece of the base tier once -- and
// of the vulkan tier, when the core has it -- so a broken install, export or
// include path fails here rather than in a sibling. Nothing here needs a GPU.

#include <cstdio>
#include <string_view>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/version.hpp"

#ifdef VKC_CONSUMER_HAS_VULKAN
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
    std::fprintf(stderr, "package consumer failed: %s\n",
                 status.message().c_str());
    return 1;
  }
  std::printf("volumetric_kit_core %s consumed\n", vkc::version_string());
  return 0;
}
