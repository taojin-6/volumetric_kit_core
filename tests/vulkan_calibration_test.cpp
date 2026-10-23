// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Interpose only the calibrated-timestamps lookup in this executable. The
// actual driver still records and resolves the spans, while the calibration
// callback can fail deterministically without a production test hook.

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace {

std::atomic<bool> fail_calibration{false};
std::atomic<unsigned> calibration_calls{0};
PFN_vkGetCalibratedTimestampsEXT driver_calibrate = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL
calibrate(VkDevice device, std::uint32_t count,
          const VkCalibratedTimestampInfoEXT* infos, std::uint64_t* stamps,
          std::uint64_t* deviation) {
  ++calibration_calls;
  if (fail_calibration) return VK_ERROR_OUT_OF_HOST_MEMORY;
  return driver_calibrate(device, count, infos, stamps, deviation);
}

}  // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name) {
  static const auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      dlsym(RTLD_NEXT, "vkGetDeviceProcAddr"));
  if (get == nullptr) return nullptr;
  const PFN_vkVoidFunction found = get(device, name);
  if (found != nullptr &&
      (std::strcmp(name, "vkGetCalibratedTimestampsKHR") == 0 ||
       std::strcmp(name, "vkGetCalibratedTimestampsEXT") == 0)) {
    driver_calibrate =
        reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(found);
    return reinterpret_cast<PFN_vkVoidFunction>(&calibrate);
  }
  return found;
}

namespace volumetric_kit::core {
namespace {

class CalibrationTest : public test::VulkanTest {
 protected:
  void TearDown() override {
    fail_calibration = false;
    VulkanTest::TearDown();
  }
};

TEST_F(CalibrationTest, FailedRefreshDiscardsExpiredPairAndCanRecover) {
  Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  Device device = *std::move(made);
  const Result<ClockPair> supported = device.calibrate();
  if (!supported.ok()) {
    ASSERT_EQ(supported.status().domain(), Status::Code::Unsupported);
    GTEST_SKIP() << "the device has no calibrated timestamps";
  }
  ASSERT_NE(driver_calibrate, nullptr)
      << "calibration lookup was not intercepted";
  Result<GpuTimer> made_timer = GpuTimer::create(device, 1);
  ASSERT_TRUE(made_timer.ok()) << made_timer.status().message();
  GpuTimer timer = *std::move(made_timer);
  if (!timer.available()) GTEST_SKIP() << "the queue has no timestamps";

  const auto span = [&]() {
    GpuTiles tiles;
    const Status submitted =
        device.submit_single_time([&](VkCommandBuffer cmd) {
          tiles = GpuTiles::begin(timer, cmd, 1);
          tiles.mark(cmd, "clock");
          tiles.end(cmd);
        });
    EXPECT_TRUE(submitted.ok()) << submitted.message();
    tiles.resolve();
    return tiles.span();
  };
  const std::optional<DeviceSpan> first = span();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first.value_or(DeviceSpan{}).host_clock);
  const unsigned initial_calls = calibration_calls;
  fail_calibration = true;
  const std::optional<DeviceSpan> cached = span();
  ASSERT_TRUE(cached.has_value());
  EXPECT_TRUE(cached.value_or(DeviceSpan{}).host_clock);
  EXPECT_EQ(calibration_calls.load(), initial_calls);

  std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
  for (unsigned failures = 1; failures <= 2; ++failures) {
    const std::optional<DeviceSpan> failed = span();
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(calibration_calls.load(), initial_calls + failures);
    const DeviceSpan failed_span = failed.value_or(DeviceSpan{});
    EXPECT_FALSE(failed_span.host_clock);
    EXPECT_EQ(failed_span.begin_ns, 0);
    EXPECT_EQ(failed_span.end_ns, 0);
    EXPECT_EQ(failed_span.deviation_ns, 0U);
  }

  fail_calibration = false;
  const std::optional<DeviceSpan> recovered = span();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_TRUE(recovered.value_or(DeviceSpan{}).host_clock);
  EXPECT_EQ(calibration_calls.load(), initial_calls + 3);
}

}  // namespace
}  // namespace volumetric_kit::core
