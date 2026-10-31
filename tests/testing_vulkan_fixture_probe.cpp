// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Probes of core_test_support's fixture that need a process of their own: the
// vkc_testing_* CTest entries (CMakeLists.txt) run these, each with its own
// filter and environment, and read the outcome from the output.

#include <string>
#include <type_traits>
#include <utility>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/two_family_device.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "vulkan_fixture_faults.hpp"

namespace volumetric_kit::core {
namespace {

// Makes a buffer on `device` and never destroys it, for the layer to report as
// the device goes.
void leave_a_buffer(VkDevice device) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = 64;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateBuffer(device, &info, nullptr, &buffer), VK_SUCCESS);
}

class Probe : public test::VulkanDeviceTest {};

// The fixture's SetUp alone, which the entries starve of a device or a layer.
TEST_F(Probe, GetsADevice) { EXPECT_NE(device().handle(), VK_NULL_HANDLE); }

// Run in one process, these two share the device: the second destroys the
// fence the first made on it.
VkDevice g_device = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

TEST_F(Probe, MakesAFence) {
  VkFenceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  ASSERT_EQ(vkCreateFence(device().handle(), &info, nullptr, &g_fence),
            VK_SUCCESS);
  g_device = device().handle();
}

TEST_F(Probe, DestroysTheFenceOnTheSameDevice) {
  ASSERT_NE(g_fence, VK_NULL_HANDLE) << "run after Probe.MakesAFence";
  ASSERT_EQ(device().handle(), g_device);
  vkDestroyFence(device().handle(), g_fence, nullptr);
}

// Leaves a buffer on the shared device, which the layer reports as that
// device is destroyed after the last test: the run fails, naming no test.
class LeakProbe : public Probe {
 protected:
  test::Validation validation() const override { return test::Validation::On; }
};

TEST_F(LeakProbe, LeavesABuffer) {
  if (!instance().validation_logged()) {
    GTEST_SKIP() << "the validation layer is not installed, or does not load";
  }
  leave_a_buffer(device().handle());
}

// A fixture chain whose middle SetUp fails after the base's succeeded: the
// leaf's SetUp returns rather than use what the middle never made.
class MiddleProbe : public Probe {
 protected:
  void SetUp() override {
    Probe::SetUp();
    if (base_setup_incomplete()) return;
    FAIL() << "the middle SetUp stops here";
  }
};

class LeafProbe : public MiddleProbe {
 protected:
  void SetUp() override {
    MiddleProbe::SetUp();
    if (base_setup_incomplete()) return;
    ADD_FAILURE() << "the leaf SetUp carried on";
  }
};

TEST_F(LeafProbe, StopsWhereTheMiddleStopped) {
  ADD_FAILURE() << "the body ran after SetUp failed";
}

// --- the two-family fixture --------------------------------------------------

class TwoFamilyProbe : public test::SharedDeviceTest {};

// Run in one process, these two borrow one two-family device.
VkDevice g_two_family = VK_NULL_HANDLE;

TEST_F(TwoFamilyProbe, MakesTheDevice) {
  g_two_family = two_family().shared().device();
  ASSERT_NE(g_two_family, VK_NULL_HANDLE);
}

TEST_F(TwoFamilyProbe, BorrowsTheSameDevice) {
  ASSERT_NE(g_two_family, VK_NULL_HANDLE) << "run after MakesTheDevice";
  EXPECT_EQ(two_family().shared().device(), g_two_family);
}

// Run in one process after MakesTheDevice, fixtures that differ from it in
// either library's requirements, or in level, get two-family devices of their
// own, and one that does not shares it. Each difference is one every device
// meets: it changes the key, not the device.
class ComputeKeyProbe : public test::SharedDeviceTest {
 protected:
  DeviceRequirements compute_requirements() const override {
    DeviceRequirements reqs;
    reqs.timeline_semaphore = false;
    return reqs;
  }
};

TEST_F(ComputeKeyProbe, GetsADeviceOfItsOwn) {
  ASSERT_NE(g_two_family, VK_NULL_HANDLE) << "run after MakesTheDevice";
  EXPECT_NE(two_family().shared().device(), g_two_family);
}

class GraphicsKeyProbe : public test::SharedDeviceTest {
 protected:
  DeviceRequirements graphics_requirements() const override {
    DeviceRequirements reqs;
    reqs.queue_flags = VK_QUEUE_GRAPHICS_BIT;
    reqs.timeline_semaphore = false;
    return reqs;
  }
};

TEST_F(GraphicsKeyProbe, GetsADeviceOfItsOwn) {
  ASSERT_NE(g_two_family, VK_NULL_HANDLE) << "run after MakesTheDevice";
  EXPECT_NE(two_family().shared().device(), g_two_family);
}

class SyncKeyProbe : public test::SharedDeviceTest {
 protected:
  test::Validation validation() const override {
    return test::Validation::Sync;
  }
};

// Run without VKC_TEST_SYNC_VALIDATION, which would raise MakesTheDevice's
// level to this one.
TEST_F(SyncKeyProbe, GetsADeviceOfItsOwn) {
  ASSERT_NE(g_two_family, VK_NULL_HANDLE) << "run after MakesTheDevice";
  ASSERT_LT(test::requested_validation(), test::Validation::Sync)
      << "run without VKC_TEST_SYNC_VALIDATION";
  if (!two_family().shared().instance().validation_logged()) {
    GTEST_SKIP() << "the validation layer is not installed, or does not load: "
                    "both levels are off";
  }
  EXPECT_NE(two_family().shared().device(), g_two_family);
}

class SameKeyProbe : public test::SharedDeviceTest {};

TEST_F(SameKeyProbe, SharesTheDevice) {
  ASSERT_NE(g_two_family, VK_NULL_HANDLE) << "run after MakesTheDevice";
  EXPECT_EQ(two_family().shared().device(), g_two_family);
}

// Leaves a buffer on the compute library's device, which the layer reports as
// the two-family device is destroyed after the last test.
TEST_F(TwoFamilyProbe, LeavesABuffer) {
  if (!two_family().shared().instance().validation_logged()) {
    GTEST_SKIP() << "the validation layer is not installed, or does not load";
  }
  leave_a_buffer(two_family().compute().handle());
}

// Both libraries asking for a queue that does graphics and compute: on a
// device where one family alone does both, the shared device has that family
// only, and the fixture skips before the body.
class OneFamilyProbe : public test::SharedDeviceTest {
 protected:
  DeviceRequirements compute_requirements() const override { return both(); }
  DeviceRequirements graphics_requirements() const override { return both(); }

 private:
  static DeviceRequirements both() {
    DeviceRequirements reqs;
    reqs.queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    return reqs;
  }
};

TEST_F(OneFamilyProbe, SkipsBeforeTheBody) {
  ADD_FAILURE() << "the body ran on " << two_family().shared().summary();
}

// Requirements the fixture cannot share, or serve headless: SetUp fails.
class ChainedTwoFamilyProbe : public test::SharedDeviceTest {
 protected:
  DeviceRequirements compute_requirements() const override {
    static VkPhysicalDeviceVulkan11Features features = [] {
      VkPhysicalDeviceVulkan11Features v11{};
      v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
      return v11;
    }();
    DeviceRequirements reqs;
    reqs.feature_chain = &features;
    return reqs;
  }
};

TEST_F(ChainedTwoFamilyProbe, FailsSetUp) { ADD_FAILURE() << "the body ran"; }

class PresentingTwoFamilyProbe : public test::SharedDeviceTest {
 protected:
  DeviceRequirements graphics_requirements() const override {
    DeviceRequirements reqs;
    reqs.queue_flags = VK_QUEUE_GRAPHICS_BIT;
    reqs.needs_present = true;
    return reqs;
  }
};

TEST_F(PresentingTwoFamilyProbe, FailsSetUp) {
  ADD_FAILURE() << "the body ran";
}

// --- devices made again ------------------------------------------------------

// What a fixture step reports, intercepted rather than failing the test.
struct Reported {
  std::string failures;  // their messages, run together
  bool skipped = false;
  std::string skip;  // why it skipped
};

template <typename Step>
Reported intercept(Step&& step) {
  ::testing::TestPartResultArray results;
  {
    const ::testing::ScopedFakeTestPartResultReporter reporter(&results);
    std::forward<Step>(step)();
  }
  Reported reported;
  for (int i = 0; i < results.size(); ++i) {
    const ::testing::TestPartResult& result = results.GetTestPartResult(i);
    if (result.skipped()) {
      reported.skipped = true;
      reported.skip += result.message();
    } else {
      reported.failures += result.message();
    }
  }
  return reported;
}

// A fixture whose test can lose its device. TearDown checks that it reports
// the injected loss, rather than fail on it, so the loss fails nothing else.
template <typename Fixture>
class Losing : public Fixture {
 protected:
  void TearDown() override {
    if (!losing_) {
      Fixture::TearDown();
      return;
    }
    const Reported reported = intercept([this] { Fixture::TearDown(); });
    EXPECT_NE(reported.failures.find("an injected device loss"),
              std::string::npos)
        << reported.failures;
  }

  // TearDown finds the device lost.
  void lose_it() {
    losing_ = true;
    test::detail::fixture_faults().lost = true;
  }

 private:
  bool losing_ = false;
};

// Run in one process, in this order, on either fixture: the first device made
// fails, the next test makes it, loses it and fails the next creation, and the
// test after that makes it again. The tests check what SetUp reports, and
// TearDown the injected loss, rather than fail on them; a skip stays a skip.
template <typename Fixture>
class Retried : public Losing<Fixture> {
 protected:
  static void SetUpTestSuite() {
    test::detail::fixture_faults().failed_creations = 1;
  }
  static void TearDownTestSuite() { test::detail::fixture_faults() = {}; }

  void SetUp() override {
    const Reported reported = intercept([this] { Fixture::SetUp(); });
    if (reported.skipped) GTEST_SKIP() << reported.skip;
    setup_failures_ = reported.failures;
  }

  // Whether SetUp made the device, failing nothing.
  ::testing::AssertionResult made() const {
    if (!setup_failures_.empty()) {
      return ::testing::AssertionFailure() << setup_failures_;
    }
    if (this->base_setup_incomplete()) {
      return ::testing::AssertionFailure() << "SetUp stopped short";
    }
    return ::testing::AssertionSuccess();
  }

  // Whether SetUp failed for the injected failure.
  ::testing::AssertionResult failed() const {
    if (setup_failures_.find("an injected failure") == std::string::npos) {
      return ::testing::AssertionFailure()
             << "SetUp reported no injected failure: " << setup_failures_;
    }
    return ::testing::AssertionSuccess();
  }

  // TearDown finds the device lost, and the next device made fails.
  void lose_it() {
    Losing<Fixture>::lose_it();
    test::detail::fixture_faults().failed_creations = 1;
  }

 private:
  std::string setup_failures_;
};

// Names each instantiation for its fixture's device, as a probe.
class DeviceNames {
 public:
  template <typename Fixture>
  static std::string GetName(int /*index*/) {
    return std::is_same_v<Fixture, test::VulkanDeviceTest> ? "DeviceProbe"
                                                           : "TwoFamilyProbe";
  }
};

using RetriedFixtures =
    ::testing::Types<test::VulkanDeviceTest, test::SharedDeviceTest>;
TYPED_TEST_SUITE(Retried, RetriedFixtures, DeviceNames);

TYPED_TEST(Retried, FailsToMakeTheDevice) { EXPECT_TRUE(this->failed()); }

TYPED_TEST(Retried, MakesItNextTime) {
  ASSERT_TRUE(this->made());
  this->lose_it();
}

TYPED_TEST(Retried, FailsToMakeItAgain) { EXPECT_TRUE(this->failed()); }

TYPED_TEST(Retried, MakesItAgain) { EXPECT_TRUE(this->made()); }

// --- a lost device -----------------------------------------------------------

// Run in one process, in this order: the first test allows validation errors,
// leaves a buffer on the device and loses it, and the device goes with that
// test, which the layer's report of the buffer alone fails; the next test gets
// a new device and passes.
class LossProbe : public Losing<Probe> {
 protected:
  test::Validation validation() const override { return test::Validation::On; }

  void SetUp() override {
    Probe::SetUp();
    if (base_setup_incomplete()) return;
    if (!instance().validation_logged()) {
      GTEST_SKIP() << "the validation layer is not installed, or does not "
                      "load";
    }
  }
};

TEST_F(LossProbe, LeavesABufferAndLosesTheDevice) {
  allow_validation_errors();
  leave_a_buffer(device().handle());
  lose_it();
}

TEST_F(LossProbe, GetsANewDevice) {
  EXPECT_NE(device().handle(), VK_NULL_HANDLE);
}

class TwoFamilyLossProbe : public Losing<test::SharedDeviceTest> {
 protected:
  void SetUp() override {
    SharedDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    if (!two_family().shared().instance().validation_logged()) {
      GTEST_SKIP() << "the validation layer is not installed, or does not "
                      "load";
    }
  }
};

TEST_F(TwoFamilyLossProbe, LeavesABufferAndLosesTheDevice) {
  allow_validation_errors();
  leave_a_buffer(two_family().compute().handle());
  lose_it();
}

TEST_F(TwoFamilyLossProbe, GetsANewDevice) {
  EXPECT_NE(two_family().shared().device(), VK_NULL_HANDLE);
}

// An instance a test makes itself, from test::instance_config(): run under
// VKC_TEST_SYNC_VALIDATION, the environment's settings reach it, it reports
// a write-after-write hazard, and the layer takes the settings without a
// complaint.
TEST(OwnInstanceProbe, RunsTheEnvironmentsSynchronizationValidation) {
  ASSERT_GE(test::requested_validation(), test::Validation::Sync)
      << "run under VKC_TEST_SYNC_VALIDATION=1";
  const test::LogCapture log;
  Result<Instance> instance = Instance::create(test::instance_config());
  if (!instance || !instance->validation_logged()) {
    GTEST_SKIP() << "no instance with the validation layer";
  }
  Result<PhysicalDeviceInfo> physical = instance->select_physical_device();
  if (!physical) GTEST_SKIP() << physical.status().message();
  Result<Device> device = Device::create(*instance, *physical, {});
  ASSERT_TRUE(device.ok()) << device.status().message();
  Result<Allocator> allocator = Allocator::create(instance->handle(), *device);
  ASSERT_TRUE(allocator.ok()) << allocator.status().message();
  BufferDesc desc;
  desc.size = 64;
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Result<Buffer> made = allocator->create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer buffer = *std::move(made);
  const int before = log.errors();
  const Status submitted = device->submit_single_time([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 1);
    vkCmdFillBuffer(cmd, buffer.handle(), 0, desc.size, 2);
  });
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  EXPECT_GT(log.errors(), before)
      << "the environment's synchronization validation did not reach an "
         "instance from test::instance_config()";
  for (const std::string& warning : log.warnings()) {
    EXPECT_EQ(warning.find("cannot be mixed"), std::string::npos) << warning;
  }
}

}  // namespace
}  // namespace volumetric_kit::core
