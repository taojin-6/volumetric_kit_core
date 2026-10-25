// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: a HostRing slot's buffers and its lease, which
// CommandBatch binds. Not installed.

#include <atomic>
#include <cstdint>

#include "volumetric_kit/core/vulkan/buffer.hpp"

namespace volumetric_kit::core::detail {

// In place, `host` is the buffer the kernels bind and the host maps. Copied,
// `host` is the slot's staging and `mirror` the device-only buffer the
// kernels bind. `held` is set while a lease is out: HostRing::claim sets it,
// and the lease's last copy clears it with a release store as it goes.
struct RingSlot {
  Buffer host;
  Buffer mirror;  // empty in place
  std::atomic<bool> held{false};
};

// The word every whole word of an output's range holds before the batch
// runs, in test builds (CommandBatch::output).
inline constexpr std::uint32_t kRingOutputPoison = 0xBADC0FFEU;

}  // namespace volumetric_kit::core::detail
