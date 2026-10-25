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
/// allocator; @ref volumetric_kit::core::test::SharedDeviceTest gives a
/// cross-family test a compute library's and a renderer's devices on two
/// queue families (`two_family_device.hpp`). All apply the environment's
/// policy (`vulkan_policy.hpp`): a missing device skips or fails, a missing
/// layer fails, and every error the layer reports fails the running test.
///
/// A fixture may ask for more validation than the environment does
/// (@ref volumetric_kit::core::test::VulkanTest::validation); it then gets it
/// wherever the layer is installed.
///
/// The environment also poisons every ring output in the process, so a result
/// word a kernel skips shows (@ref volumetric_kit::core::CommandBatch::output).
///
/// The tests in a process share one instance per validation level, one device
/// per level and set of requirements, and one two-family device, with an
/// instance of its own, per level and pair of requirements: the first test
/// that needs one makes it, and it is destroyed after the last test. Each test
/// makes and destroys its own objects on them. An object a test never destroys
/// is reported as the shared device is destroyed, failing the run rather than
/// a named test.
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

#include "volumetric_kit/core/testing/two_family_device.hpp"  // IWYU pragma: export
#include "volumetric_kit/core/testing/vulkan_policy.hpp"  // IWYU pragma: export
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"

namespace volumetric_kit::core::test {

namespace detail {
struct DeviceSlot;
struct TwoFamilySlot;
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

  // Each completes SetUp in two steps.
  friend class VulkanDeviceTest;
  friend class SharedDeviceTest;

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

/// @brief A @ref VulkanTest with the process's @ref TwoFamilyDevice for its
///        two requirements: a compute library's device and a renderer's, on
///        one headless `SharedDevice`, each with an allocator.
///
/// It skips where the shared device puts both libraries on one queue family
/// -- two queues of one family, which `SharedDevice::create` prefers, or one
/// shared queue -- so a test on it always crosses families. It runs under the
/// validation layer wherever the layer is installed, as the fixture asks for
/// @ref Validation::On by default.
///
/// The two-family device brings its own instance. The fixture's instance and
/// physical device only decide, for both requirements merged, whether to skip
/// or fail without a device, so they are hidden from a test on it: use
/// `two_family().shared().instance()` and `.physical()`.
///
/// The two-family device is shared as @ref VulkanDeviceTest's device is, and
/// requirements carrying a `feature_chain` fail `SetUp` for the same reason,
/// as does a renderer that presents: there is no surface. `TearDown` waits
/// for both queues to go idle; a test that loses the device fails, and the
/// next one gets a new one.
///
/// @code
/// class DepthCloudCrossFamily : public test::SharedDeviceTest {
///  protected:
///   DeviceRequirements graphics_requirements() const override {
///     return gfx::device_requirements();
///   }
/// };
///
/// TEST_F(DepthCloudCrossFamily, DrawsWhatComputeWrote) {
///   test::TwoFamilyDevice& two = two_family();
///   BufferDesc desc;
///   desc.size = bytes;
///   desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
///                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
///   desc.queue_families = two.families().data();
///   desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
///   Result<Buffer> depth = two.compute_allocator().create_buffer(desc);
///   ASSERT_TRUE(depth.ok()) << depth.status().message();
///   ASSERT_TRUE(two.write_buffer(*depth, 0, metres.data(), bytes).ok());
///   // ... draw *depth on two.graphics(); a validation error fails this test.
/// }
/// @endcode
class SharedDeviceTest : public VulkanTest {
 public:
  ~SharedDeviceTest() override;

 protected:
  SharedDeviceTest();

  /// @return The compute library's requirements; the defaults unless a
  ///         fixture overrides it.
  virtual DeviceRequirements compute_requirements() const { return {}; }
  /// @return The renderer's requirements; a graphics queue unless a fixture
  ///         overrides it.
  virtual DeviceRequirements graphics_requirements() const;
  /// @return @ref Validation::On, unless a fixture overrides it.
  Validation validation() const override { return Validation::On; }
  /// @return Both requirements merged, with each library's queue capability,
  ///         as `SharedDevice::create` merges them: what the fixture's
  ///         physical device must meet. A `feature_chain` and presenting,
  ///         which `SetUp` refuses, are left out.
  DeviceRequirements requirements() const final;

  /// @brief @ref VulkanTest::SetUp, then borrow the shared two-family device;
  ///        skips where it has one family.
  void SetUp() override;
  /// @brief Wait for both queues to go idle.
  void TearDown() override;

  /// @return The process's two-family device for these requirements, which
  ///         crosses families. @pre `SetUp` completed.
  TwoFamilyDevice& two_family();

 private:
  // Another instance than the two-family device's.
  using VulkanTest::instance;
  using VulkanTest::physical;

  detail::TwoFamilySlot* slot_ = nullptr;  // to mark it lost
  TwoFamilyDevice* device_ = nullptr;
};

namespace detail {

/// @brief Register the process-wide setup and teardown around the tests: the
///        layer's settings, the ring-output poison, and the destruction of the
///        shared instances and devices.
/// @return The registered environment, which GoogleTest owns.
::testing::Environment* register_vulkan_environment() noexcept;

/// Registered before `main` runs the tests, once per test binary however
/// many of its sources include this header.
inline ::testing::Environment* const kVulkanEnvironment =
    register_vulkan_environment();

}  // namespace detail

}  // namespace volumetric_kit::core::test
