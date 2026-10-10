// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file vulkan_fixture.hpp
/// @brief The GoogleTest fixtures the family's Vulkan tests build on
///        (`volumetric_kit::core_test_support`).
///
/// @ref volumetric_kit::core::test::VulkanTest gives a test an instance and
/// the best physical device for its requirements;
/// @ref volumetric_kit::core::test::VulkanDeviceTest adds a device and an
/// allocator. Both apply the environment's policy (`vulkan_policy.hpp`): a
/// missing device skips or fails, a missing layer fails, and every error the
/// layer reports fails the running test.
///
/// A fixture may ask for more validation than the environment does
/// (@ref volumetric_kit::core::test::VulkanTest::validation); it then gets it
/// wherever the layer is installed.
///
/// The tests in a process share one instance per validation level, and one
/// device per level and set of requirements: the first test that needs one
/// makes it, and it is destroyed after the last test. Each test makes and
/// destroys its own objects on them. An object a test never destroys is
/// reported as the shared device is destroyed, failing the run rather than a
/// named test.
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// class UploadTest : public test::VulkanDeviceTest {
///  protected:
///   // Barrier-heavy: run under synchronization validation wherever the
///   // layer is installed, not only when the environment asks.
///   test::Validation validation() const override {
///     return test::Validation::Sync;
///   }
/// };
///
/// TEST_F(UploadTest, RoundTrips) {
///   Result<Buffer> buffer = allocator().create_buffer(desc);
///   ASSERT_TRUE(buffer.ok()) << buffer.status().message();
///   // ... record on device(); a validation error fails this test.
/// }
/// @endcode

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/testing/vulkan_policy.hpp"  // IWYU pragma: export
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"

namespace volumetric_kit::core::test {

namespace detail {
struct DeviceSlot;
}  // namespace detail

/// @brief A test with an instance and a physical device: shared, validated
///        and required as the file comment describes.
///
/// A test makes whatever it needs on them, devices included. From `SetUp`
/// until the fixture is destroyed, the log handler is a @ref LogCapture whose
/// `"vulkan"` errors fail the test. A test that installs its own handler
/// gives that up.
///
/// @code
/// class DeviceTest : public test::VulkanTest {};
///
/// TEST_F(DeviceTest, MakesADevice) {
///   Result<Device> device = Device::create(instance(), physical(), {});
///   ASSERT_TRUE(device.ok()) << device.status().message();
/// }
/// @endcode
class VulkanTest : public ::testing::Test {
 public:
  ~VulkanTest() override;

 protected:
  VulkanTest();

  /// @return The requirements the physical device -- and
  ///         @ref VulkanDeviceTest's device -- must meet; the defaults unless
  ///         a fixture overrides it.
  virtual DeviceRequirements requirements() const { return {}; }
  /// @return The validation the fixture asks for, whatever the environment
  ///         asks; it runs at the higher of the two. Off unless a fixture
  ///         overrides it. Unlike the environment's, the fixture's request
  ///         is met only where the layer is installed.
  virtual Validation validation() const { return Validation::Off; }

  /// @brief Borrow the shared instance, install the log handler, and select
  ///        the physical device; skips or fails without either.
  void SetUp() override;

  /// @return Whether a `SetUp` so far in the fixture chain -- the base's, or
  ///         an intermediate fixture's -- stopped short, skipping or failing
  ///         the test and leaving members unset. A derived `SetUp` returns
  ///         when it is true: `ASSERT_*` and `GTEST_SKIP` return only from
  ///         the function they fire in, and `IsSkipped()` is false for a test
  ///         that has also failed.
  bool base_setup_incomplete() const {
    return !ready_ || HasFatalFailure() || IsSkipped();
  }

  /// @return The shared instance. @pre `SetUp` completed.
  const Instance& instance() const;
  /// @return The selected physical device, as `select_physical_device`
  ///         captured it. @pre `SetUp` completed.
  const PhysicalDeviceInfo& physical() const { return physical_; }
  /// @return The validation the instance was made for: the environment's or
  ///         the fixture's, whichever is higher.
  Validation active_validation() const { return validation_; }

  /// @brief For a test that commits invalid usage on purpose: from here on,
  ///        its `"vulkan"` errors are counted instead of failing it.
  void allow_validation_errors();
  /// @return The `"vulkan"` errors reported since
  ///         @ref allow_validation_errors.
  int allowed_validation_errors() const;
  /// @return The warnings logged since `SetUp` began.
  std::vector<std::string> warnings() const;

 private:
  struct Capture;

  friend class VulkanDeviceTest;  // completes SetUp in two steps

  std::unique_ptr<Capture> capture_;
  const Instance* instance_ = nullptr;
  PhysicalDeviceInfo physical_;
  Validation validation_ = Validation::Off;
  bool ready_ = false;  // SetUp completed
};

/// @brief A @ref VulkanTest with the shared device for its requirements, and
///        an allocator of its own on it.
///
/// The device is shared with every test in the process at the same
/// validation level whose requirements are equal, so requirements carrying
/// a `feature_chain`, which cannot be compared, fail `SetUp`: make such a
/// device in the test, on a @ref VulkanTest. `TearDown` waits for the device
/// to go idle, leaving it ready for the next test; a test that loses the
/// device fails, and the next one gets a new device. A derived fixture's
/// members may hold objects made on the device or the allocator: the device
/// outlives them, and the allocator is destroyed after them.
///
/// @code
/// class ResourcesTest : public test::VulkanDeviceTest {};
///
/// TEST_F(ResourcesTest, MakesABuffer) {
///   BufferDesc desc;
///   desc.size = 256;
///   desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
///   EXPECT_TRUE(allocator().create_buffer(desc).ok());
/// }
/// @endcode
class VulkanDeviceTest : public VulkanTest {
 public:
  ~VulkanDeviceTest() override;

 protected:
  VulkanDeviceTest();

  /// @brief @ref VulkanTest::SetUp, then borrow the shared device and make
  ///        the allocator.
  void SetUp() override;
  /// @brief Wait for the device to go idle.
  void TearDown() override;

  /// @return The shared device. @pre `SetUp` completed.
  Device& device();
  /// @return This test's allocator. @pre `SetUp` completed.
  Allocator& allocator();

 private:
  detail::DeviceSlot* slot_ = nullptr;  // the shared device's, to mark it lost
  Device* device_ = nullptr;
  std::optional<Allocator> allocator_;
};

namespace detail {

/// @brief Register the process-wide setup and teardown around the tests: the
///        layer's settings, and the destruction of the shared instances and
///        devices.
/// @return The registered environment, which GoogleTest owns.
::testing::Environment* register_vulkan_environment() noexcept;

/// Registered before `main` runs the tests, once per test binary however
/// many of its sources include this header.
inline ::testing::Environment* const kVulkanEnvironment =
    register_vulkan_environment();

}  // namespace detail

}  // namespace volumetric_kit::core::test
