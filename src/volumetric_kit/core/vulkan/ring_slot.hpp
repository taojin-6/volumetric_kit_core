// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: a HostRing slot's buffers and its lease, which
// CommandBatch binds, and the poison a test process writes into a ring
// output. Not installed.

#include <atomic>
#include <cstdint>

#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"

namespace volumetric_kit::core::detail {

// In place, `host` is the buffer the kernels bind and the host maps. Copied,
// `host` is the slot's staging and `mirror` the device-only buffer the
// kernels bind. `held` is set while a lease is out: HostRing::claim sets it,
// and the lease's last copy clears it with a release store as it goes.
// `write_only` is set by a HostAccess::SequentialWrite ring at create, before
// any lease, and never changes: CommandBatch::output refuses such a slot.
struct RingSlot {
  Buffer host;
  Buffer mirror;  // empty in place
  std::atomic<bool> held{false};
  bool write_only = false;
};

// The word every whole word of an output's range holds before the batch
// runs, while the poison is on (CommandBatch::output).
inline constexpr std::uint32_t kRingOutputPoison = 0xBADC0FFEU;

// Whether CommandBatch::output poisons its range. Off unless a test process
// switches it on: the test policy's RingOutputPoison does while it lives
// (vulkan_policy.cpp), so an app or probe never records the poison. Exported
// for that policy and the tests.
VKC_VULKAN_API void set_ring_output_poison(bool on) noexcept;
VKC_VULKAN_API bool ring_output_poison() noexcept;

}  // namespace volumetric_kit::core::detail
