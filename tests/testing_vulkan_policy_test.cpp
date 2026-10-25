// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_policy without GoogleTest, as a test that is a main() of its own
// uses it: a ValidationSession's settings reach an instance made from
// instance_config(), and a LogCapture counts the hazard the layer reports and
// nothing else; under a RingOutputPoison, a ring output no kernel writes holds
// the poison word. A missing device or layer ends it as the environment asks;
// CTest registers kSkipExitCode as its SKIP_RETURN_CODE.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/host_ring.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;

namespace {

// Synchronization validation wherever the layer is installed, not only when
// the environment asks for it.
test::Validation level() {
  return std::max(test::requested_validation(), test::Validation::Sync);
}

// Runs a batch that binds a ring output and nothing writes it: main's
// RingOutputPoison poisons with no GoogleTest environment, so every word reads
// as test::kRingOutputPoison.
int check_poison(const vkc::Device& device, vkc::Allocator& allocator) {
  std::array<std::uint32_t, 4> words{};
  vkc::HostRingDesc desc;
  desc.slot_bytes = sizeof(words);
  desc.slots = 1;
  vkc::Result<std::unique_ptr<vkc::HostRing>> ring =
      vkc::HostRing::create(device, allocator, desc);
  if (!ring) {
    std::fprintf(stderr, "HostRing::create: %s\n",
                 ring.status().message().c_str());
    return 1;
  }
  vkc::Result<vkc::HostSlot> slot = (*ring)->claim(sizeof(words));
  if (!slot) {
    std::fprintf(stderr, "claim: %s\n", slot.status().message().c_str());
    return 1;
  }
  std::memset(slot->data(), 0, sizeof(words));
  vkc::CommandBatch batch(device, allocator);
  if (const vkc::Result<VkBuffer> out = batch.output(*slot, 0, sizeof(words));
      !out) {
    std::fprintf(stderr, "output: %s\n", out.status().message().c_str());
    return 1;
  }
  if (const vkc::Status submitted = batch.submit(); !submitted.ok()) {
    std::fprintf(stderr, "submit: %s\n", submitted.message().c_str());
    return 1;
  }
  std::memcpy(words.data(), slot->data(), sizeof(words));
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (words[i] != test::kRingOutputPoison) {
      std::fprintf(stderr, "ring output word %zu is 0x%08X, not the poison\n",
                   i, words[i]);
      return 1;
    }
  }
  return 0;
}

// Records two writes of the same bytes with no barrier between -- write after
// write, which only synchronization validation reports -- and sets `hazards`
// to the errors logged meanwhile. Its objects are destroyed before it
// returns, while `log` still counts.
int run(const test::LogCapture& log, int& hazards) {
  vkc::Result<vkc::Instance> instance =
      vkc::Instance::create(test::instance_config(level()));
  if (!instance) {
    return test::no_device_exit_code("no Vulkan instance: " +
                                     instance.status().message());
  }
  if (const vkc::Status loaded = test::check_layer_loaded(*instance);
      !loaded.ok()) {
    std::fprintf(stderr, "%s\n", loaded.message().c_str());
    return 1;
  }
  if (!instance->validation_logged()) {
    std::fprintf(stderr,
                 "the validation layer is not installed, or does not load; "
                 "skipping\n");
    return test::kSkipExitCode;
  }
  vkc::Result<vkc::PhysicalDeviceInfo> physical =
      instance->select_physical_device();
  if (!physical) return test::no_device_exit_code(physical.status().message());
  vkc::Result<vkc::Device> device =
      vkc::Device::create(*instance, *physical, {});
  if (!device) {
    std::fprintf(stderr, "Device::create: %s\n",
                 device.status().message().c_str());
    return 1;
  }
  vkc::Result<vkc::Allocator> allocator =
      vkc::Allocator::create(instance->handle(), *device);
  if (!allocator) {
    std::fprintf(stderr, "Allocator::create: %s\n",
                 allocator.status().message().c_str());
    return 1;
  }
  vkc::BufferDesc desc;
  desc.size = 64;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkc::Result<vkc::Buffer> made = allocator->create_buffer(desc);
  if (!made) {
    std::fprintf(stderr, "create_buffer: %s\n",
                 made.status().message().c_str());
    return 1;
  }
  const vkc::Buffer buffer = *std::move(made);
  const int before = log.errors();
  const vkc::Status submitted =
      device->submit_single_time([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 1);
        vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 2);
      });
  if (!submitted.ok()) {
    std::fprintf(stderr, "submit: %s\n", submitted.message().c_str());
    return 1;
  }
  hazards = log.errors() - before;
  return check_poison(*device, *allocator);
}

}  // namespace

int main() {
  const test::ValidationSession validation(level());
  const test::RingOutputPoison poison;
  const test::LogCapture log;
  int hazards = 0;
  const int code = run(log, hazards);
  if (code != 0) return code;
  if (hazards == 0) {
    std::fprintf(stderr,
                 "no hazard reported: the session's synchronization "
                 "validation did not reach the instance\n");
    return 1;
  }
  if (log.errors() != hazards) {
    std::fprintf(stderr, "the layer reported more than the hazard\n");
    return 1;
  }
  std::printf("the layer reported the hazard, and nothing else\n");
  return 0;
}
