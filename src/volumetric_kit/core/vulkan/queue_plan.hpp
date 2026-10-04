// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: how SharedDevice carves the two libraries' queues
// out of a device's queue families. Pulled out to be tested on its own, on
// family layouts no test machine has -- a family with no queues, a transfer
// or video family beside the one that does everything, a compute family that
// presents. Not installed.

#include <cstdint>
#include <optional>
#include <vector>

#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

// The plan taken, and the family each library's queue comes from.
struct QueueCarving {
  QueuePlan plan = QueuePlan::SharedQueue;
  std::uint32_t graphics_family = 0;
  std::uint32_t compute_family = 0;
};

// The best plan `families` allow: the renderer's family has every
// `graphics_flags` bit and presents (`presents[i]`, every entry true without
// a surface), the compute library's has every `compute_flags` bit, and a
// family with no queues is never taken. Every plan is searched in turn, best
// first: a device whose families hold a queue each (MoltenVK) has no first
// plan, and stopping at the first family that does both would take the last
// plan and lose independent submission. Empty when no family can serve the
// renderer beside one that serves the compute library.
inline std::optional<QueueCarving> choose_queue_plan(
    const std::vector<VkQueueFamilyProperties>& families,
    VkQueueFlags graphics_flags, VkQueueFlags compute_flags,
    const std::vector<bool>& presents) {
  const auto count = static_cast<std::uint32_t>(families.size());
  const auto has = [&](std::uint32_t i, VkQueueFlags flags) {
    return families[i].queueCount > 0 &&
           (families[i].queueFlags & flags) == flags;
  };
  const auto serves_renderer = [&](std::uint32_t i) {
    return has(i, graphics_flags) && i < presents.size() && presents[i];
  };
  for (std::uint32_t i = 0; i < count; ++i) {
    if (serves_renderer(i) && has(i, compute_flags) &&
        families[i].queueCount >= 2) {
      return QueueCarving{QueuePlan::TwoQueuesOneFamily, i, i};
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!serves_renderer(i)) continue;
    for (std::uint32_t j = 0; j < count; ++j) {
      if (j != i && has(j, compute_flags)) {
        return QueueCarving{QueuePlan::TwoFamilies, i, j};
      }
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (serves_renderer(i) && has(i, compute_flags)) {
      return QueueCarving{QueuePlan::SharedQueue, i, i};
    }
  }
  return std::nullopt;
}

}  // namespace volumetric_kit::core::detail
