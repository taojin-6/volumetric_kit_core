// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// SharedDevice: one device for a compute library and a renderer, each adopting
// it. Windowless, so it runs where the other device tests do; the plan it
// takes depends on the device (MoltenVK: TwoFamilies; a single-queue driver:
// SharedQueue), and the validation layer checks the queues' locking.

#include "volumetric_kit/core/vulkan/shared_device.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "vulkan_fixture.hpp"

namespace volumetric_kit::core {
namespace {

SharedDeviceConfig windowless() {
  SharedDeviceConfig config;
  config.instance.app_name = "volumetric_kit_core shared device test";
  config.instance.enable_validation = test::env_set("VKC_TEST_VALIDATION");
  config.compute = DeviceRequirements{};  // a compute queue, timelines
  config.graphics.queue_flags = VK_QUEUE_GRAPHICS_BIT;
  return config;
}

// --- before any device
// --------------------------------------------------------

TEST(SharedDeviceArgs, APresentingRendererNeedsASurfaceMaker) {
  SharedDeviceConfig config = windowless();
  config.graphics.needs_present = true;
  EXPECT_EQ(SharedDevice::create(config).status().domain(),
            Status::Code::InvalidArgument);
}

TEST(SharedDeviceArgs, RequirementsThatCannotMergeAreRefused) {
  // Two opaque feature chains cannot be merged: neither can be read.
  VkPhysicalDeviceVulkan11Features a{};
  a.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  VkPhysicalDeviceVulkan11Features b = a;
  SharedDeviceConfig config = windowless();
  config.compute.feature_chain = &a;
  config.graphics.feature_chain = &b;
  EXPECT_EQ(SharedDevice::create(config).status().domain(),
            Status::Code::InvalidArgument);
}

TEST(QueuePlanNames, NameEachPlan) {
  EXPECT_STREQ(to_string(QueuePlan::TwoQueuesOneFamily), "TwoQueuesOneFamily");
  EXPECT_STREQ(to_string(QueuePlan::TwoFamilies), "TwoFamilies");
  EXPECT_STREQ(to_string(QueuePlan::SharedQueue), "SharedQueue");
}

// --- on a device
// ----------------------------------------------------------------

// VulkanTest skips without a device and counts validation errors from the
// shared device's instance too: they reach the same log sink.
using SharedDeviceTest = test::VulkanTest;

TEST_F(SharedDeviceTest, ASurfaceMakersFailureIsReturned) {
  SharedDeviceConfig config = windowless();
  config.graphics.needs_present = true;
  VkInstance seen = VK_NULL_HANDLE;
  config.make_surface = [&](VkInstance instance) -> Result<VkSurfaceKHR> {
    seen = instance;
    return Status::unsupported("no window in this test");
  };
  const Status s = SharedDevice::create(config).status();
  EXPECT_EQ(s.domain(), Status::Code::Unsupported);
  EXPECT_NE(s.message().find("no window"), std::string::npos);
  EXPECT_NE(seen, VK_NULL_HANDLE);  // made on the shared instance
}

TEST_F(SharedDeviceTest, ADeviceLackingTheUnionIsNamed) {
  SharedDeviceConfig config = windowless();
  config.graphics.extensions = {"VK_VKC_no_such_extension"};
  const Status s = SharedDevice::create(config).status();
  EXPECT_FALSE(s.ok());
  EXPECT_NE(s.message().find("VK_VKC_no_such_extension"), std::string::npos)
      << s.message();
}

TEST_F(SharedDeviceTest, BuildsOneDeviceBothLibrariesAdopt) {
  const SharedDeviceConfig config = windowless();
  Result<std::unique_ptr<SharedDevice>> made = SharedDevice::create(config);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const std::unique_ptr<SharedDevice> shared = *std::move(made);
  ASSERT_NE(shared->device(), VK_NULL_HANDLE);
  EXPECT_EQ(shared->surface(), VK_NULL_HANDLE);
  EXPECT_EQ(shared->release_surface(), VK_NULL_HANDLE);
  const std::string summary = shared->summary();
  EXPECT_NE(summary.find(to_string(shared->plan())), std::string::npos);
  EXPECT_NE(summary.find(shared->physical().properties().deviceName),
            std::string::npos);

  const AdoptedDevice compute = shared->compute_payload();
  const AdoptedDevice graphics = shared->graphics_payload();
  EXPECT_EQ(compute.device, shared->device());
  EXPECT_EQ(graphics.device, shared->device());
  EXPECT_EQ(compute.queue_family, shared->compute_family());
  EXPECT_EQ(graphics.queue_family, shared->graphics_family());
  ASSERT_NE(compute.submit_mutex, nullptr);
  ASSERT_NE(graphics.submit_mutex, nullptr);
  EXPECT_FALSE(graphics.has_present);  // no surface
  switch (shared->plan()) {
    case QueuePlan::TwoQueuesOneFamily:
      EXPECT_EQ(compute.queue_family, graphics.queue_family);
      EXPECT_NE(compute.queue, graphics.queue);
      EXPECT_NE(compute.submit_mutex, graphics.submit_mutex);
      break;
    case QueuePlan::TwoFamilies:
      EXPECT_NE(compute.queue_family, graphics.queue_family);
      EXPECT_NE(compute.submit_mutex, graphics.submit_mutex);
      break;
    case QueuePlan::SharedQueue:
      EXPECT_EQ(compute.queue, graphics.queue);
      EXPECT_EQ(compute.submit_mutex, graphics.submit_mutex);
      break;
  }
  const auto families =
      static_cast<std::uint32_t>(shared->physical().queue_families().size());
  if (families > 1) {
    // A device with several families never settles for one shared queue.
    EXPECT_NE(shared->plan(), QueuePlan::SharedQueue) << summary;
  }

  // Each library adopts with its own requirements, which the declaration
  // must meet -- read back from creation, not restated.
  Result<Device> fusion_made = Device::adopt(compute, config.compute);
  ASSERT_TRUE(fusion_made.ok()) << fusion_made.status().message();
  Result<Device> renderer_made = Device::adopt(graphics, config.graphics);
  ASSERT_TRUE(renderer_made.ok()) << renderer_made.status().message();
  const Device fusion = *std::move(fusion_made);
  const Device renderer = *std::move(renderer_made);
  EXPECT_FALSE(fusion.owns_device());
  EXPECT_EQ(fusion.queue(), compute.queue);
  EXPECT_EQ(renderer.queue(), graphics.queue);
  EXPECT_EQ(fusion.submit_mutex(), compute.submit_mutex);
}

// The point of one device: a buffer the compute library writes is the
// renderer's to read in place. Named for both families, it is CONCURRENT
// exactly when they differ.
TEST_F(SharedDeviceTest, ABufferCrossesBetweenTheLibrariesInPlace) {
  const SharedDeviceConfig config = windowless();
  Result<std::unique_ptr<SharedDevice>> made = SharedDevice::create(config);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const std::unique_ptr<SharedDevice> shared = *std::move(made);
  Result<Device> fusion_made =
      Device::adopt(shared->compute_payload(), config.compute);
  Result<Device> renderer_made =
      Device::adopt(shared->graphics_payload(), config.graphics);
  ASSERT_TRUE(fusion_made.ok() && renderer_made.ok());
  const Device fusion = *std::move(fusion_made);
  const Device renderer = *std::move(renderer_made);
  Result<Allocator> fusion_allocator =
      Allocator::create(shared->instance().handle(), fusion);
  Result<Allocator> renderer_allocator =
      Allocator::create(shared->instance().handle(), renderer);
  ASSERT_TRUE(fusion_allocator.ok() && renderer_allocator.ok());

  constexpr std::uint32_t kWords = 512;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kWords} * 4;
  const std::uint32_t both[2] = {shared->compute_family(),
                                 shared->graphics_family()};
  BufferDesc desc;
  desc.size = kBytes;
  desc.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  desc.queue_families = both;
  desc.queue_family_count = 2;
  Result<Buffer> made_mesh = fusion_allocator->create_buffer(desc);
  ASSERT_TRUE(made_mesh.ok()) << made_mesh.status().message();
  const Buffer mesh = *std::move(made_mesh);
  EXPECT_EQ(mesh.sharing_mode(), both[0] != both[1]
                                     ? VK_SHARING_MODE_CONCURRENT
                                     : VK_SHARING_MODE_EXCLUSIVE);

  std::vector<std::uint32_t> words(kWords);
  for (std::uint32_t i = 0; i < kWords; ++i) words[i] = (i * 31U) + 7U;
  {
    CommandBatch write(fusion, *fusion_allocator);
    ASSERT_TRUE(write.upload(mesh, 0, words.data(), kBytes).ok());
    ASSERT_TRUE(write.submit().ok());
  }
  std::vector<std::uint32_t> drawn(kWords, 0);
  {
    CommandBatch read(renderer, *renderer_allocator);
    ASSERT_TRUE(read.readback(mesh, 0, kBytes, drawn.data()).ok());
    ASSERT_TRUE(read.submit().ok());
  }
  EXPECT_EQ(drawn, words);
}

// The two libraries submit from their own threads while a third drains: the
// mutexes keep each queue's operations apart, which the validation layer
// checks.
TEST_F(SharedDeviceTest, BothLibrariesSubmitWhileItDrains) {
  const SharedDeviceConfig config = windowless();
  Result<std::unique_ptr<SharedDevice>> made = SharedDevice::create(config);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const std::unique_ptr<SharedDevice> shared = *std::move(made);
  Result<Device> fusion_made =
      Device::adopt(shared->compute_payload(), config.compute);
  Result<Device> renderer_made =
      Device::adopt(shared->graphics_payload(), config.graphics);
  ASSERT_TRUE(fusion_made.ok() && renderer_made.ok());
  const Device fusion = *std::move(fusion_made);
  const Device renderer = *std::move(renderer_made);

  constexpr int kRounds = 40;
  std::atomic<int> failures{0};
  std::atomic<bool> done{false};
  const auto submit_rounds = [&](const Device& device) {
    for (int r = 0; r < kRounds; ++r) {
      if (!device.submit_single_time([](VkCommandBuffer) {}).ok()) ++failures;
    }
  };
  std::thread drainer([&] {
    while (!done.load()) shared->wait_idle();
  });
  std::thread compute([&] { submit_rounds(fusion); });
  std::thread graphics([&] { submit_rounds(renderer); });
  compute.join();
  graphics.join();
  done = true;
  drainer.join();
  EXPECT_EQ(failures.load(), 0);
  shared->wait_idle();
}

}  // namespace
}  // namespace volumetric_kit::core
