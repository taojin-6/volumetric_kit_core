// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file gpu_test.hpp
/// @brief What a GPU test that is a `main()` of its own runs on: a device
///        that meets the test's requirements, under the environment's test
///        policy, with no test framework (`volumetric_kit::core_test_policy`).
///
/// `VKC_REQUIRE_VULKAN_DEVICE`, `VKC_TEST_VALIDATION` and
/// `VKC_TEST_SYNC_VALIDATION` mean here what they mean for the fixtures
/// (vulkan_policy.hpp): every error the validation layer reports fails the
/// test, and a test skips by returning @ref kSkipExitCode, which its
/// `SKIP_RETURN_CODE` reports as a skip. A @ref RingOutputPoison poisons
/// every ring output before its batch runs, so a result word a kernel skips
/// shows (`CommandBatch::output`).
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// int gpu_main(test::GpuContext& gpu) {
///   // ... make, test and destroy everything on gpu.device
///   return 0;
/// }
/// int main() { return test::run_on_gpu(my_library_requirements(), gpu_main); }
/// @endcode

#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/buffer_readback.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

/// @brief A @ref Gpu, with the instance and the physical device its device
///        was made on, and the run's log capture: a test reads the warnings
///        it keeps rather than installing a handler, which would end the
///        capture.
///
/// @code
/// int gpu_main(test::GpuContext& gpu) {
///   std::printf("%s\n", gpu.physical.properties().deviceName);
///   return gpu.log.warnings().empty() ? 0 : 1;
/// }
/// @endcode
struct GpuContext : Gpu {
  Instance& instance;                  ///< The instance.
  const PhysicalDeviceInfo& physical;  ///< The device's physical device.
  const LogCapture& log;               ///< The run's capture.
};

namespace detail {

// Makes the context and runs `test` on it. Everything made here is destroyed
// before this returns, while `log` still counts what the validation layer
// reports, a leaked object included.
template <typename Test>
int run_on_gpu(const DeviceRequirements& requirements, Test& test,
               const LogCapture& log) {
  Result<Instance> instance = Instance::create(instance_config());
  if (!instance) {
    return no_device_exit_code("no Vulkan instance (" +
                               instance.status().message() + ")");
  }
  if (const Status loaded = check_layer_loaded(*instance); !loaded.ok()) {
    std::fprintf(stderr, "%s\n", loaded.message().c_str());
    return 1;
  }
  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance->handle(), &count, nullptr);
  if (count == 0) return no_device_exit_code("no physical devices");
  // A device that falls short of the requirements fails rather than skips:
  // they are the requirements the test states a real driver meets.
  Result<PhysicalDeviceInfo> physical =
      instance->select_physical_device(requirements);
  if (!physical) {
    std::fprintf(stderr,
                 "%u device(s), but none meets the test's requirements: %s\n",
                 count, physical.status().message().c_str());
    return 1;
  }
  Result<Device> device = Device::create(*instance, *physical, requirements);
  if (!device) {
    std::fprintf(stderr, "device create failed on %s: %s\n",
                 physical->properties().deviceName,
                 device.status().message().c_str());
    return 1;
  }
  Result<Allocator> allocator = Allocator::create(instance->handle(), *device);
  if (!allocator) {
    std::fprintf(stderr, "allocator create failed: %s\n",
                 allocator.status().message().c_str());
    return 1;
  }
  // The test takes it by non-const reference, which the dependent call hides
  // from the check.
  // NOLINTNEXTLINE(misc-const-correctness)
  GpuContext gpu{{*device, *allocator}, *instance, *physical, log};
  return test(gpu);
}

}  // namespace detail

/// @brief Run a test on a device that meets @p requirements, under the
///        validation the environment asks for.
/// @tparam Test         An `int(GpuContext&)`, returning the test's exit code.
/// @param requirements  What the device must provide; a device that falls
///                      short fails the test rather than skips it.
/// @param test          The test.
/// @param on_error      Takes each error the layer reports; stderr does
///                      without one.
/// @return The process's exit code: the test's, or 1 where the layer reported
///         an error; @ref kSkipExitCode, or 1 under
///         `VKC_REQUIRE_VULKAN_DEVICE`, where there is no device.
template <typename Test>
int run_on_gpu(const DeviceRequirements& requirements, Test&& test,
               LogCapture::ErrorHandler on_error = {}) {
  const ValidationSession validation;
  const RingOutputPoison poison;
  const LogCapture log(std::move(on_error));
  return log.exit_code(detail::run_on_gpu(requirements, test, log));
}

/// @brief A queue family of @p device's GPU besides the device's own, which a
///        `CONCURRENT` resource may name with it.
/// @param device  The device.
/// @return The family; none on a GPU with one family.
inline std::optional<std::uint32_t> other_queue_family(const Device& device) {
  if (device.caps().queue_families().size() < 2) return std::nullopt;
  return device.queue_family() == 0 ? 1u : 0u;
}

}  // namespace volumetric_kit::core::test
