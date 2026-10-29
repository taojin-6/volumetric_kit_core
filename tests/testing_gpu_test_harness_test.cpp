// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_policy's run_on_gpu (gpu_test.hpp) under the Khronos validation
// layer: a test the layer reports nothing for passes, one that skips skips,
// and one that returns 0 fails when the layer reports an error, whether in the
// test's own calls or for an object the test leaked, which the layer reports
// as the harness destroys the device; under synchronization validation, a
// missing barrier fails it too. A ring output no kernel writes reads as the
// poison. The device meets the requirements given: one asking for a feature
// enables it, and one no device meets fails without running the test.
// other_queue_family names a family besides the device's own where the GPU
// has one. Skips where the layer or a device is missing, unless
// VKC_REQUIRE_VULKAN_DEVICE is set.
//
// With --no-device, run where the loader finds no driver
// (tests/CMakeLists.txt): run_on_gpu skips without running the test, and
// fails under VKC_REQUIRE_VULKAN_DEVICE.
//
// Under LeakSanitizer on Linux, with the sanitizer job's suppressions
// (tools/lsan.supp), a block leaked where the layer reports an error -- in the
// error handler the core's debug messenger callback reaches, under the
// loader's and the layer's frames -- is reported.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

#ifdef __has_feature
#if __has_feature(address_sanitizer)
#define VKC_TEST_LSAN 1
#endif
#endif
#ifdef __SANITIZE_ADDRESS__
#define VKC_TEST_LSAN 1
#endif
#if defined(VKC_TEST_LSAN) && defined(__linux__)
#include <sanitizer/lsan_interface.h>
#else
#undef VKC_TEST_LSAN
#endif

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/gpu_test.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/host_ring.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;

namespace {

int passes(test::GpuContext& /*gpu*/) { return 0; }

int skips(test::GpuContext& /*gpu*/) { return test::kSkipExitCode; }

// A buffer with no usage, which the layer refuses before the driver sees it.
int misuses(test::GpuContext& gpu) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = 256;
  info.usage = 0;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(gpu.device.handle(), &info, nullptr, &buffer) ==
      VK_SUCCESS) {
    vkDestroyBuffer(gpu.device.handle(), buffer, nullptr);
  }
  return 0;
}

// A semaphore never destroyed, which the layer reports only as the device is.
int leaks(test::GpuContext& gpu) {
  VkSemaphoreCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkSemaphore semaphore = VK_NULL_HANDLE;
  // 2, not 1, so a failure here is not taken for the layer's report.
  if (vkCreateSemaphore(gpu.device.handle(), &info, nullptr, &semaphore) !=
      VK_SUCCESS) {
    return 2;
  }
  return 0;
}

// Two fills of one buffer with no barrier between them: a write after a
// write, which synchronization validation reports and plain validation does
// not.
int races(test::GpuContext& gpu) {
  vkc::Result<vkc::Buffer> buffer =
      vkc::device_storage_buffer(gpu.allocator, 256);
  if (!buffer) return 2;
  VkBuffer handle = buffer->handle();
  const vkc::Status submitted =
      gpu.device.submit_single_time([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, handle, 0, VK_WHOLE_SIZE, 1);
        vkCmdFillBuffer(cmd, handle, 0, VK_WHOLE_SIZE, 2);
      });
  return submitted.ok() ? 0 : 2;
}

// A batch that binds a ring output and writes none of it: every word reads as
// the poison, on the device's own path, in place or copied.
int poisons(test::GpuContext& gpu) {
  vkc::HostRingDesc desc;
  desc.slot_bytes = 16;
  desc.name = "test.poisoned";
  vkc::Result<std::unique_ptr<vkc::HostRing>> ring =
      vkc::HostRing::create(gpu.device, gpu.allocator, desc);
  CHECK(ring.ok());
  vkc::Result<vkc::HostSlot> slot = (*ring)->claim(16);
  CHECK(slot.ok());
  std::memset(slot->data(), 0, 16);
  vkc::CommandBatch batch(gpu.device, gpu.allocator);
  CHECK(batch.output(*slot, 0, 16).ok() && batch.submit().ok());
  std::uint32_t words[4] = {};
  std::memcpy(words, slot->data(), sizeof(words));
  for (const std::uint32_t word : words) {
    CHECK(word == test::kRingOutputPoison);
  }
  return 0;
}

// Asks for scalarBlockLayout, which the defaults leave off: the device the
// test gets has it enabled.
vkc::DeviceRequirements scalar_layout() {
  vkc::DeviceRequirements requirements;
  requirements.scalar_block_layout = true;
  return requirements;
}

// The test runs, on a device made for the requirements it was given.
int honours_requirements() {
  bool ran = false;
  const int scalar =
      test::run_on_gpu(scalar_layout(), [&](test::GpuContext& gpu) {
        ran = true;
        return gpu.device.check_enabled(scalar_layout()).ok() ? 0 : 2;
      });
  CHECK(scalar == 0);
  CHECK(ran);
  // The defaults' device does not have it, so the check above is not
  // vacuous.
  CHECK(test::run_on_gpu({}, [](test::GpuContext& gpu) {
          return gpu.device.check_enabled(scalar_layout()).ok() ? 2 : 0;
        }) == 0);
  // No device has this extension: the test fails without running.
  vkc::DeviceRequirements unmet;
  unmet.extensions = {"VK_VKC_no_such_extension"};
  ran = false;
  CHECK(test::run_on_gpu(unmet, [&](test::GpuContext& /*gpu*/) {
          ran = true;
          return 0;
        }) == 1);
  CHECK(!ran);
  return 0;
}

// Another family where the GPU has one, and never the device's own.
int names_other_family(test::GpuContext& gpu) {
  const std::optional<std::uint32_t> other =
      test::other_queue_family(gpu.device);
  if (gpu.device.caps().queue_families().size() < 2) {
    CHECK(!other.has_value());
    return 0;
  }
  CHECK(other.has_value());
  CHECK(*other != gpu.device.queue_family());
  CHECK(*other < gpu.device.caps().queue_families().size());
  return 0;
}

// Where the loader finds no driver: a skip, with the test never run, and a
// failure where a device is required.
int without_a_device() {
  bool ran = false;
  const auto test_body = [&](test::GpuContext& /*gpu*/) {
    ran = true;
    return 0;
  };
  {
    const test::ScopedEnv optional("VKC_REQUIRE_VULKAN_DEVICE", nullptr);
    CHECK(test::run_on_gpu({}, test_body) == test::kSkipExitCode);
  }
  {
    const test::ScopedEnv required("VKC_REQUIRE_VULKAN_DEVICE", "1");
    CHECK(test::run_on_gpu({}, test_body) == 1);
  }
  CHECK(!ran);
  return 0;
}

#ifdef VKC_TEST_LSAN
// The block leak_a_block leaks, its address XORed so that LSan's scan of the
// globals finds no pointer to it.
constexpr std::uintptr_t kHide =
    static_cast<std::uintptr_t>(0xa5a5a5a5a5a5a5a5u);
volatile std::uintptr_t hidden_block = 0;

void leak_a_block(std::string_view /*message*/) {
  if (hidden_block == 0) {
    hidden_block = reinterpret_cast<std::uintptr_t>(std::malloc(64)) ^ kHide;
  }
}

// Overwrites the stack the leaking call used, so that no stale copy of the
// block's address there makes it reachable. Uninstrumented, so the array is
// on the stack rather than ASan's fake stack.
__attribute__((noinline, no_sanitize("address"))) void scrub_stack() {
  volatile unsigned char scratch[256 * 1024];
  for (volatile unsigned char& byte : scratch) byte = 0;
}

int lsan_reports_leak_in_error_handler() {
  scrub_stack();
  CHECK(__lsan_do_recoverable_leak_check() == 0);  // none before the test's
  CHECK(test::run_on_gpu({}, misuses, leak_a_block) == 1);
  CHECK(hidden_block != 0);
  scrub_stack();
  const bool reported = __lsan_do_recoverable_leak_check() != 0;
  std::free(reinterpret_cast<void*>(hidden_block ^ kHide));
  CHECK(reported);
  return 0;
}
#endif

}  // namespace

// An exception that escapes aborts the test, which fails it.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--no-device") {
    if (without_a_device() != 0) return 1;
    std::puts("gpu_test_harness --no-device: OK");
    return 0;
  }
  if (test::validation_layer_version() == 0) {
    return test::no_device_exit_code("no Khronos validation layer");
  }
  const test::ScopedEnv validation("VKC_TEST_VALIDATION", "1");
  const int clean = test::run_on_gpu({}, passes);
  if (clean == test::kSkipExitCode) return clean;  // no device
  CHECK(clean == 0);
  CHECK(test::run_on_gpu({}, poisons) == 0);
  CHECK(test::run_on_gpu({}, skips) == test::kSkipExitCode);
  CHECK(test::run_on_gpu({}, misuses) == 1);
  CHECK(test::run_on_gpu({}, leaks) == 1);
  if (honours_requirements() != 0) return 1;
  CHECK(test::run_on_gpu({}, names_other_family) == 0);
  {
    const test::ScopedEnv plain("VKC_TEST_SYNC_VALIDATION", nullptr);
    CHECK(test::run_on_gpu({}, races) == 0);
  }
  const test::ScopedEnv sync("VKC_TEST_SYNC_VALIDATION", "1");
  CHECK(test::run_on_gpu({}, races) == 1);
#ifdef VKC_TEST_LSAN
  if (lsan_reports_leak_in_error_handler() != 0) return 1;
#endif
  std::puts("gpu_test_harness: OK");
  return 0;
}
