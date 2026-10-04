// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Instance, PhysicalDeviceInfo, check_device_support and Device on a real
// device (or lavapipe). Skipped with no device, unless
// VKC_REQUIRE_VULKAN_DEVICE is set; see vulkan_fixture.hpp.

#include "volumetric_kit/core/vulkan/device.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "vulkan_fixture.hpp"

namespace volumetric_kit::core {
namespace {

constexpr const char* kNoSuchExtension = "VK_VKC_no_such_extension";
constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";

class DeviceTest : public test::VulkanTest {};

// A compute-to-compute memory barrier: valid on any compute queue, and enough
// for a submit to have something to do.
void record_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
}

std::vector<std::string> supported_extensions(VkPhysicalDevice physical) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> props(count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, props.data());
  std::vector<std::string> names;
  names.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    names.emplace_back(props[i].extensionName);
  }
  return names;
}

// --- Instance and selection
// ----------------------------------------------------

TEST_F(DeviceTest, InstanceReportsWhatItNegotiated) {
  EXPECT_NE(instance().handle(), VK_NULL_HANDLE);
  EXPECT_GE(instance().api_version(), VK_API_VERSION_1_1);
  EXPECT_LE(instance().api_version(), VK_API_VERSION_1_3);
}

TEST_F(DeviceTest, SelectionSaysWhyNoDeviceQualifies) {
  DeviceRequirements missing;
  missing.extensions = {kNoSuchExtension};
  const Result<VkPhysicalDevice> none =
      instance().select_physical_device(missing);
  EXPECT_EQ(none.status().domain(), Status::Code::Unsupported);
  EXPECT_NE(none.status().message().find(kNoSuchExtension), std::string::npos)
      << none.status().message();

  DeviceRequirements future;
  future.api_version = VK_MAKE_API_VERSION(0, 9, 0, 0);
  EXPECT_EQ(instance().select_physical_device(future).status().domain(),
            Status::Code::Unsupported);

  DeviceRequirements present;
  present.needs_present = true;  // and no surface
  EXPECT_EQ(instance().select_physical_device(present).status().domain(),
            Status::Code::InvalidArgument);
}

TEST_F(DeviceTest, CapsDescribeTheSelectedDevice) {
  const PhysicalDeviceInfo caps = PhysicalDeviceInfo::query(physical());
  EXPECT_EQ(caps.handle(), physical());
  EXPECT_GE(caps.api_version(), VK_API_VERSION_1_2);
  EXPECT_FALSE(caps.queue_families().empty());
  EXPECT_TRUE(caps.supports_timeline_semaphore());
  EXPECT_FALSE(caps.supports_device_extension(nullptr));
  EXPECT_FALSE(caps.supports_device_extension(kNoSuchExtension));

  const Result<DeviceSupport> support = check_device_support(caps, {});
  ASSERT_TRUE(support.ok()) << support.status().message();
  EXPECT_NE(caps.queue_families()[support->queue_family].queueFlags &
                VK_QUEUE_COMPUTE_BIT,
            0u);
  EXPECT_FALSE(support->present_family.has_value());
}

// --- create
// ----------------------------------------------------------------------

TEST_F(DeviceTest, CreatesAnOwnedDeviceWithAComputeQueue) {
  const Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Device& device = *made;
  EXPECT_NE(device.handle(), VK_NULL_HANDLE);
  EXPECT_EQ(device.physical_device(), physical());
  EXPECT_TRUE(device.owns_device());
  EXPECT_NE(device.queue(), VK_NULL_HANDLE);
  EXPECT_NE(device.queue_flags() & VK_QUEUE_COMPUTE_BIT, 0u);
  EXPECT_EQ(device.caps().handle(), physical());
  EXPECT_FALSE(device.has_present());
  EXPECT_NE(device.submit_mutex(), nullptr);
}

TEST_F(DeviceTest, RefusesAMissingRequiredExtension) {
  DeviceRequirements reqs;
  reqs.extensions = {kNoSuchExtension};
  const Result<Device> made = Device::create(instance(), physical(), reqs);
  EXPECT_EQ(made.status().domain(), Status::Code::Unsupported);
  EXPECT_NE(made.status().message().find(kNoSuchExtension), std::string::npos);
}

TEST_F(DeviceTest, RefusesRequirementsAboveTheInstancesVersion) {
  DeviceRequirements reqs;
  reqs.api_version = VK_MAKE_API_VERSION(0, 1, 4, 0);  // the instance asks 1.3
  const Result<Device> made = Device::create(instance(), physical(), reqs);
  EXPECT_EQ(made.status().domain(), Status::Code::Unsupported);
  EXPECT_NE(made.status().message().find("instance negotiated"),
            std::string::npos)
      << made.status().message();
}

TEST_F(DeviceTest, EnablesOptionalExtensionsOnlyWhereOffered) {
  DeviceRequirements reqs;
  reqs.optional_extensions = {kNoSuchExtension};
  const Result<Device> made = Device::create(instance(), physical(), reqs);
  ASSERT_TRUE(made.ok()) << made.status().message();
  EXPECT_FALSE(made->extension_enabled(kNoSuchExtension));
  // The spec requires the portability subset wherever it is exposed.
  EXPECT_EQ(made->extension_enabled(kPortabilitySubset),
            made->caps().supports_device_extension(kPortabilitySubset));
  EXPECT_FALSE(made->extension_enabled(nullptr));
}

TEST_F(DeviceTest, RaisesFeatureBitsInTheCallersChain) {
  if (instance().api_version() < VK_API_VERSION_1_2) {
    GTEST_SKIP() << "VkPhysicalDeviceVulkan12Features needs a 1.2 instance";
  }
  const PhysicalDeviceInfo caps = PhysicalDeviceInfo::query(physical());
  VkPhysicalDeviceVulkan12Features v12{};
  v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  DeviceRequirements reqs;
  reqs.scalar_block_layout = caps.supports_scalar_block_layout();
  reqs.feature_chain = &v12;
  // Linking a standalone timeline struct beside the aggregate would break
  // VUID-VkDeviceCreateInfo-pNext-02830, which the validation leg catches.
  const Result<Device> made = Device::create(instance(), physical(), reqs);
  ASSERT_TRUE(made.ok()) << made.status().message();
  EXPECT_EQ(v12.timelineSemaphore, VK_TRUE);
  EXPECT_EQ(v12.scalarBlockLayout,
            reqs.scalar_block_layout ? VK_TRUE : VK_FALSE);
}

// --- submit
// ----------------------------------------------------------------------

TEST_F(DeviceTest, SubmitsAndWaits) {
  const Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  int recorded = 0;
  for (int i = 0; i < 3; ++i) {  // the second and third reuse the command
    const Status s = made->submit_single_time([&](VkCommandBuffer cmd) {
      record_barrier(cmd);
      ++recorded;
    });
    ASSERT_TRUE(s.ok()) << s.message();
  }
  EXPECT_EQ(recorded, 3);
  EXPECT_TRUE(made->wait_idle().ok());
}

TEST_F(DeviceTest, SubmitsFromManyThreads) {
  const Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  constexpr int kThreads = 8;
  constexpr int kSubmits = 25;
  std::atomic<int> recorded{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kSubmits; ++i) {
        const Status s = made->submit_single_time([&](VkCommandBuffer cmd) {
          record_barrier(cmd);
          ++recorded;
        });
        if (!s) ++failed;
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(failed.load(), 0);
  EXPECT_EQ(recorded.load(), kThreads * kSubmits);
}

TEST_F(DeviceTest, RecordMaySubmitOnTheSameDevice) {
  const Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  Status inner;
  const Status outer = made->submit_single_time([&](VkCommandBuffer cmd) {
    inner = made->submit_single_time(record_barrier);
    record_barrier(cmd);
  });
  EXPECT_TRUE(outer.ok()) << outer.message();
  EXPECT_TRUE(inner.ok()) << inner.message();
}

TEST_F(DeviceTest, SubmitsACallersCommandBuffer) {
  const Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = made->queue_family();
  VkCommandPool pool = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateCommandPool(made->handle(), &pool_info, nullptr, &pool),
            VK_SUCCESS);
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  ASSERT_EQ(vkAllocateCommandBuffers(made->handle(), &alloc, &cmd), VK_SUCCESS);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin), VK_SUCCESS);
  record_barrier(cmd);
  ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS);

  EXPECT_TRUE(made->submit_and_wait(cmd).ok());
  EXPECT_TRUE(made->submit_and_wait(cmd).ok());  // not ONE_TIME_SUBMIT
  EXPECT_EQ(made->submit_and_wait(VK_NULL_HANDLE).domain(),
            Status::Code::InvalidArgument);
  vkDestroyCommandPool(made->handle(), pool, nullptr);
}

TEST_F(DeviceTest, LabelsAndNamesAreSafeWithOrWithoutDebugUtils) {
  const Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  if (!instance().debug_utils_enabled()) {
    EXPECT_FALSE(made->debug_labels_available());
  }
  made->set_object_name(VK_OBJECT_TYPE_QUEUE,
                        debug_object_handle(made->queue()), "core queue");
  made->set_object_name(VK_OBJECT_TYPE_QUEUE,
                        debug_object_handle(made->queue()), nullptr);
  const Status s = made->submit_single_time([&](VkCommandBuffer cmd) {
    made->begin_debug_label(cmd, "outer");
    made->begin_debug_label(cmd, nullptr);  // opens nothing...
    record_barrier(cmd);
    made->end_debug_label(cmd, nullptr);  // ...so closes nothing
    made->end_debug_label(cmd, "outer");
  });
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(debug_object_handle(VK_NULL_HANDLE), 0u);
}

// --- moves
// -----------------------------------------------------------------------

TEST_F(DeviceTest, MovesLeaveTheSourceEmpty) {
  Result<Device> made = Device::create(instance(), physical(), {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  Device first = *std::move(made);
  VkDevice handle = first.handle();
  ASSERT_TRUE(first.submit_single_time(record_barrier).ok());

  Device second = std::move(first);
  // Reading the moved-from device is the point: it must be empty.
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(first.handle(), VK_NULL_HANDLE);
  EXPECT_EQ(first.queue(), VK_NULL_HANDLE);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(second.handle(), handle);
  EXPECT_TRUE(second.submit_single_time(record_barrier).ok());

  Result<Device> other = Device::create(instance(), physical(), {});
  ASSERT_TRUE(other.ok()) << other.status().message();
  Device third = *std::move(other);
  third = std::move(second);  // destroys third's own device first
  EXPECT_EQ(third.handle(), handle);
  Device* self = &third;
  third = std::move(*self);
  EXPECT_EQ(third.handle(), handle);
  EXPECT_TRUE(third.submit_single_time(record_barrier).ok());
}

// --- adopt
// -----------------------------------------------------------------------

// A device made by hand, as an embedder would: a compute queue, timeline
// semaphores, scalar block layout where offered, the portability subset
// where exposed.
struct RawDevice {
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  bool scalar = false;
  std::vector<const char*> extensions;

  RawDevice() = default;
  RawDevice(const RawDevice&) = delete;
  RawDevice& operator=(const RawDevice&) = delete;
  ~RawDevice() {
    if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
  }
};

void make_raw_device(VkPhysicalDevice physical, RawDevice* raw) {
  const PhysicalDeviceInfo caps = PhysicalDeviceInfo::query(physical);
  const Result<DeviceSupport> support = check_device_support(caps, {});
  ASSERT_TRUE(support.ok()) << support.status().message();
  raw->family = support->queue_family;
  raw->scalar = caps.supports_scalar_block_layout();
  if (caps.supports_device_extension(kPortabilitySubset)) {
    raw->extensions.push_back(kPortabilitySubset);
  }
  VkPhysicalDeviceScalarBlockLayoutFeatures scalar{};
  scalar.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
  scalar.scalarBlockLayout = raw->scalar ? VK_TRUE : VK_FALSE;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
  timeline.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  timeline.timelineSemaphore = VK_TRUE;
  timeline.pNext = &scalar;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &timeline;
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{};
  queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue.queueFamilyIndex = raw->family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.pNext = &features2;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queue;
  info.enabledExtensionCount =
      static_cast<std::uint32_t>(raw->extensions.size());
  info.ppEnabledExtensionNames =
      raw->extensions.empty() ? nullptr : raw->extensions.data();
  ASSERT_EQ(vkCreateDevice(physical, &info, nullptr, &raw->device), VK_SUCCESS);
  vkGetDeviceQueue(raw->device, raw->family, 0, &raw->queue);
}

AdoptedDevice handoff(const Instance& instance, VkPhysicalDevice physical,
                      const RawDevice& raw) {
  AdoptedDevice adopted;
  adopted.instance = instance.handle();
  adopted.physical_device = physical;
  adopted.device = raw.device;
  adopted.queue_family = raw.family;
  adopted.queue = raw.queue;
  adopted.enabled_extensions = raw.extensions.data();
  adopted.enabled_extension_count =
      static_cast<std::uint32_t>(raw.extensions.size());
  adopted.enabled_timeline_semaphore = true;
  adopted.enabled_scalar_block_layout = raw.scalar;
  adopted.enabled_debug_utils = instance.debug_utils_enabled();
  return adopted;
}

TEST_F(DeviceTest, AdoptsADeviceWithoutOwningIt) {
  RawDevice raw;
  ASSERT_NO_FATAL_FAILURE(make_raw_device(physical(), &raw));
  std::mutex shared;
  AdoptedDevice adopted = handoff(instance(), physical(), raw);
  adopted.submit_mutex = &shared;
  {
    const Result<Device> borrowed = Device::adopt(adopted, {});
    ASSERT_TRUE(borrowed.ok()) << borrowed.status().message();
    EXPECT_FALSE(borrowed->owns_device());
    EXPECT_EQ(borrowed->handle(), raw.device);
    EXPECT_EQ(borrowed->queue(), raw.queue);
    EXPECT_EQ(borrowed->submit_mutex(), &shared);
    EXPECT_TRUE(borrowed->submit_single_time(record_barrier).ok());
  }
  // The borrowed device is gone; the VkDevice is still the embedder's.
  EXPECT_EQ(vkDeviceWaitIdle(raw.device), VK_SUCCESS);
}

TEST_F(DeviceTest, AdoptLocksItsOwnMutexOnAnUnsharedQueue) {
  RawDevice raw;
  ASSERT_NO_FATAL_FAILURE(make_raw_device(physical(), &raw));
  const Result<Device> borrowed =
      Device::adopt(handoff(instance(), physical(), raw), {});
  ASSERT_TRUE(borrowed.ok()) << borrowed.status().message();
  EXPECT_NE(borrowed->submit_mutex(), nullptr);
}

TEST_F(DeviceTest, AdoptRefusesWhatWasNotDeclared) {
  RawDevice raw;
  ASSERT_NO_FATAL_FAILURE(make_raw_device(physical(), &raw));

  AdoptedDevice no_timeline = handoff(instance(), physical(), raw);
  no_timeline.enabled_timeline_semaphore = false;
  const Result<Device> refused = Device::adopt(no_timeline, {});
  EXPECT_EQ(refused.status().domain(), Status::Code::Unsupported);
  EXPECT_NE(refused.status().message().find("timelineSemaphore"),
            std::string::npos);

  // Supported by the physical device, but not enabled on this one.
  std::string undeclared;
  for (const std::string& name : supported_extensions(physical())) {
    if (name != kPortabilitySubset) {
      undeclared = name;
      break;
    }
  }
  if (!undeclared.empty()) {
    DeviceRequirements reqs;
    reqs.extensions = {undeclared};
    const Result<Device> missing =
        Device::adopt(handoff(instance(), physical(), raw), reqs);
    EXPECT_EQ(missing.status().domain(), Status::Code::Unsupported);
    EXPECT_NE(missing.status().message().find("not declared enabled"),
              std::string::npos)
        << missing.status().message();
  }

  DeviceRequirements wants_scalar;
  wants_scalar.scalar_block_layout = true;
  AdoptedDevice no_scalar = handoff(instance(), physical(), raw);
  no_scalar.enabled_scalar_block_layout = false;
  EXPECT_FALSE(Device::adopt(no_scalar, wants_scalar).ok());
}

TEST_F(DeviceTest, AdoptRefusesMalformedHandoffs) {
  RawDevice raw;
  ASSERT_NO_FATAL_FAILURE(make_raw_device(physical(), &raw));
  const AdoptedDevice good = handoff(instance(), physical(), raw);

  AdoptedDevice no_device = good;
  no_device.device = VK_NULL_HANDLE;
  EXPECT_EQ(Device::adopt(no_device, {}).status().domain(),
            Status::Code::InvalidArgument);

  AdoptedDevice far_family = good;
  far_family.queue_family = 1000;
  EXPECT_EQ(Device::adopt(far_family, {}).status().domain(),
            Status::Code::InvalidArgument);

  AdoptedDevice present_without_queue = good;
  present_without_queue.has_present = true;
  EXPECT_EQ(Device::adopt(present_without_queue, {}).status().domain(),
            Status::Code::InvalidArgument);

  DeviceRequirements wants_present;
  wants_present.needs_present = true;
  EXPECT_EQ(Device::adopt(good, wants_present).status().domain(),
            Status::Code::InvalidArgument);
}

}  // namespace
}  // namespace volumetric_kit::core
