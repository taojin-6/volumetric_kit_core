// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_policy's two-family device and core_test_support's
// SharedDeviceTest fixture on it: each library on the family SharedDevice
// carves for it, writes on the compute library's queue, a buffer and an image
// written there read back exactly on the renderer's, and the writes' own
// refusals (test::write_image's are testing_write_image_test.cpp's). What
// needs a process of its own -- one device per process and per key, a leak,
// the skip on one family, another family layout -- runs from
// testing_vulkan_fixture_probe.cpp (CMakeLists.txt).

#include "volumetric_kit/core/testing/two_family_device.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

DeviceRequirements graphics_queue() {
  DeviceRequirements graphics;
  graphics.queue_flags = VK_QUEUE_GRAPHICS_BIT;
  return graphics;
}

bool says(const Status& status, const char* what) {
  return status.message().find(what) != std::string::npos;
}

// --- the two-family device, without the fixture -----------------------------

TEST(TwoFamilyDeviceArgs, ARendererThatPresentsIsRefused) {
  DeviceRequirements presenting = graphics_queue();
  presenting.needs_present = true;
  EXPECT_EQ(test::TwoFamilyDevice::create({}, presenting).status().domain(),
            Status::Code::InvalidArgument);
}

// A VulkanTest for the policy: the two-family device's instance logs to the
// same sink, so its layer's errors fail the test too.
using TwoFamilyDeviceTest = test::VulkanTest;

TEST_F(TwoFamilyDeviceTest, CrossesFamiliesExactlyWhereSharedDeviceTakesTwo) {
  Result<std::unique_ptr<test::TwoFamilyDevice>> made =
      test::TwoFamilyDevice::create({}, graphics_queue());
  ASSERT_TRUE(made.ok()) << made.status().message();
  test::TwoFamilyDevice& two = **made;
  const SharedDevice& shared = two.shared();
  const std::string summary = shared.summary();
  EXPECT_EQ(two.crosses_families(), shared.plan() == QueuePlan::TwoFamilies)
      << summary;
  EXPECT_EQ(two.crosses_families(),
            shared.compute_family() != shared.graphics_family())
      << summary;
  const std::array<std::uint32_t, 2> families = {shared.compute_family(),
                                                 shared.graphics_family()};
  EXPECT_EQ(two.families(), families);
  // Each library adopted the one device, on the queue carved for it.
  EXPECT_EQ(two.compute().handle(), shared.device());
  EXPECT_EQ(two.graphics().handle(), shared.device());
  EXPECT_FALSE(two.compute().owns_device());
  EXPECT_EQ(two.compute().queue_family(), shared.compute_family());
  EXPECT_EQ(two.graphics().queue_family(), shared.graphics_family());
  EXPECT_NE(two.compute().queue_flags() & VK_QUEUE_COMPUTE_BIT, 0U);
  EXPECT_NE(two.graphics().queue_flags() & VK_QUEUE_GRAPHICS_BIT, 0U);
  if (two.crosses_families()) {
    EXPECT_NE(two.compute().queue(), two.graphics().queue());
  }
  // At the environment's level, which is the default.
  EXPECT_EQ(shared.instance().validation_logged(),
            test::requested_validation() != test::Validation::Off);
}

// --- the fixture -------------------------------------------------------------

// What a fixture on `Base` can call: SharedDeviceTest hides VulkanTest's
// instance and physical device, which are another instance's than the
// two-family device's. Access is checked in substitution, here from inside a
// fixture.
template <typename Base>
class Reaches : public Base {
  template <typename T, typename = void>
  struct Instance : std::false_type {};
  template <typename T>
  struct Instance<T, std::void_t<decltype(std::declval<const T&>().instance())>>
      : std::true_type {};
  template <typename T, typename = void>
  struct Physical : std::false_type {};
  template <typename T>
  struct Physical<T, std::void_t<decltype(std::declval<const T&>().physical())>>
      : std::true_type {};

 public:
  static constexpr bool kInstance = Instance<Reaches>::value;
  static constexpr bool kPhysical = Physical<Reaches>::value;
};
static_assert(Reaches<test::VulkanTest>::kInstance &&
                  Reaches<test::VulkanTest>::kPhysical,
              "a VulkanTest fixture reaches its instance and physical device");
static_assert(!Reaches<test::SharedDeviceTest>::kInstance &&
                  !Reaches<test::SharedDeviceTest>::kPhysical,
              "a SharedDeviceTest fixture reaches only the two-family "
              "device's instance and physical device");

class CrossFamilyTest : public test::SharedDeviceTest {
 protected:
  // A buffer of `bytes` on the compute library's allocator, CONCURRENT
  // across both families unless `exclusive`.
  Result<Buffer> buffer(VkDeviceSize bytes, bool exclusive = false) {
    BufferDesc desc;
    desc.size = bytes;
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!exclusive) {
      desc.queue_families = two_family().families().data();
      desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
    }
    return two_family().compute_allocator().create_buffer(desc);
  }

  // A transfer image of `extent`, as `buffer` makes a buffer.
  Result<Image> image(
      VkExtent2D extent, VkFormat format,
      VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      bool exclusive = false) {
    ImageDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.usage = usage;
    desc.with_view = false;
    if (!exclusive) {
      desc.queue_families = two_family().families().data();
      desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
    }
    return two_family().compute_allocator().create_image(desc);
  }

  // `image`'s texels, copied and read back on the renderer's queue.
  std::vector<std::uint8_t> read_on_graphics(const Image& source,
                                             VkDeviceSize bytes) {
    test::TwoFamilyDevice& two = two_family();
    BufferDesc desc;
    desc.size = bytes;
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Result<Buffer> copied = two.graphics_allocator().create_buffer(desc);
    EXPECT_TRUE(copied.ok()) << copied.status().message();
    std::vector<std::uint8_t> out(bytes, 0);
    if (!copied.ok()) return out;
    CommandBatch read(two.graphics(), two.graphics_allocator());
    EXPECT_TRUE(
        read.copy(source, source.width(), source.height(), *copied, 0).ok());
    EXPECT_TRUE(read.readback(*copied, 0, bytes, out.data()).ok());
    const Status submitted = read.submit();
    EXPECT_TRUE(submitted.ok()) << submitted.message();
    return out;
  }
};

TEST_F(CrossFamilyTest, HandsEachLibraryItsOwnFamily) {
  test::TwoFamilyDevice& two = two_family();
  EXPECT_TRUE(two.crosses_families());
  EXPECT_NE(two.compute().queue_family(), two.graphics().queue_family());
  EXPECT_EQ(two.families()[0], two.compute().queue_family());
  EXPECT_EQ(two.families()[1], two.graphics().queue_family());
  // Validated wherever the layer is installed, as the fixture asks: where
  // the fixture's own instance -- hidden from a test, so named here -- is.
  EXPECT_GE(active_validation(), test::Validation::On);
  EXPECT_EQ(two.shared().instance().validation_logged(),
            VulkanTest::instance().validation_logged());
}

// A write on the compute library's queue cannot finish while the test holds
// that queue's mutex, so one that finishes sooner went to another queue, and
// a cross-family test on it would be a same-family one.
TEST_F(CrossFamilyTest, WritesSubmitOnTheComputeLibrarysQueue) {
  test::TwoFamilyDevice& two = two_family();
  ASSERT_NE(two.compute().submit_mutex(), two.graphics().submit_mutex());
  const auto waits_for_compute =
      [&](const std::function<Status()>& write) -> ::testing::AssertionResult {
    std::unique_lock<std::mutex> hold(*two.compute().submit_mutex());
    std::atomic<bool> done{false};
    Status status;
    std::thread writer([&] {
      status = write();
      done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool early = done;
    hold.unlock();
    writer.join();
    if (early) {
      return ::testing::AssertionFailure()
             << "it completed with the compute queue held";
    }
    if (!status.ok()) return ::testing::AssertionFailure() << status.message();
    return ::testing::AssertionSuccess();
  };

  Result<Buffer> shared_buffer = buffer(64);
  ASSERT_TRUE(shared_buffer.ok()) << shared_buffer.status().message();
  const std::vector<std::uint8_t> bytes(64, 3);
  EXPECT_TRUE(waits_for_compute([&] {
    return two.write_buffer(*shared_buffer, 0, bytes.data(), 64);
  })) << "write_buffer";

  Result<Image> shared_image = image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM);
  ASSERT_TRUE(shared_image.ok()) << shared_image.status().message();
  EXPECT_TRUE(waits_for_compute([&] {
    return two.write_image(*shared_image, bytes.data(), 64,
                           VK_IMAGE_LAYOUT_GENERAL);
  })) << "write_image";
}

TEST_F(CrossFamilyTest, ABufferWrittenOnComputeReadsBackExactlyOnGraphics) {
  test::TwoFamilyDevice& two = two_family();
  // Past CommandBatch's inline limit, so the writes are staged.
  constexpr std::uint32_t kWords = 20000;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kWords} * 4;
  constexpr std::uint32_t kHalf = kWords / 2;
  Result<Buffer> made = buffer(kBytes);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Buffer shared = *std::move(made);
  EXPECT_EQ(shared.sharing_mode(), VK_SHARING_MODE_CONCURRENT);

  std::vector<std::uint32_t> first(kWords);
  std::vector<std::uint32_t> second(kHalf);
  for (std::uint32_t i = 0; i < kWords; ++i) first[i] = (i * 2654435761U) + 1;
  for (std::uint32_t i = 0; i < kHalf; ++i) second[i] = ~i;
  ASSERT_TRUE(two.write_buffer(shared, 0, first.data(), kBytes).ok());
  // The second half again, at an offset.
  const Status at = two.write_buffer(shared, VkDeviceSize{kHalf} * 4,
                                     second.data(), VkDeviceSize{kHalf} * 4);
  ASSERT_TRUE(at.ok()) << at.message();

  std::vector<std::uint32_t> read(kWords, 0);
  CommandBatch batch(two.graphics(), two.graphics_allocator());
  ASSERT_TRUE(batch.readback(shared, 0, kBytes, read.data()).ok());
  const Status submitted = batch.submit();
  ASSERT_TRUE(submitted.ok()) << submitted.message();
  std::vector<std::uint32_t> expected(first.begin(), first.begin() + kHalf);
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(read, expected);
}

TEST_F(CrossFamilyTest, AnImageWrittenOnComputeCopiesExactlyOnGraphics) {
  test::TwoFamilyDevice& two = two_family();
  // An odd width, so a row is no power of two in bytes.
  constexpr VkExtent2D kExtent = {37, 23};
  constexpr VkDeviceSize kBytes =
      VkDeviceSize{kExtent.width} * kExtent.height * 4;
  Result<Image> made = image(kExtent, VK_FORMAT_R8G8B8A8_UNORM);
  ASSERT_TRUE(made.ok()) << made.status().message();
  Image shared = *std::move(made);
  EXPECT_EQ(shared.sharing_mode(), VK_SHARING_MODE_CONCURRENT);

  std::vector<std::uint8_t> texels(kBytes);
  for (VkDeviceSize i = 0; i < kBytes; ++i) {
    texels[i] = static_cast<std::uint8_t>((i * 7) + (i / 148));
  }
  ASSERT_TRUE(two.write_image(shared, texels.data(), kBytes,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                  .ok());
  EXPECT_EQ(shared.layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(read_on_graphics(shared, kBytes), texels);

  // Written again after the renderer read it, and left GENERAL.
  for (std::uint8_t& texel : texels) texel = static_cast<std::uint8_t>(~texel);
  ASSERT_TRUE(
      two.write_image(shared, texels.data(), kBytes, VK_IMAGE_LAYOUT_GENERAL)
          .ok());
  EXPECT_EQ(shared.layout(), VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(read_on_graphics(shared, kBytes), texels);
}

TEST_F(CrossFamilyTest, WritesRefuseWhatTheRendererCouldNotRead) {
  test::TwoFamilyDevice& two = two_family();
  const std::vector<std::uint8_t> bytes(64, 1);
  const auto refused = [](const Status& status, const char* what) {
    return status.domain() == Status::Code::InvalidArgument &&
           says(status, what);
  };

  // An EXCLUSIVE buffer: the renderer's read would be undefined.
  Result<Buffer> exclusive_buffer = buffer(64, /*exclusive=*/true);
  ASSERT_TRUE(exclusive_buffer.ok());
  const Status buffer_refusal =
      two.write_buffer(*exclusive_buffer, 0, bytes.data(), 64);
  EXPECT_TRUE(refused(buffer_refusal, "EXCLUSIVE")) << buffer_refusal.message();
  // An empty one is no sharing refusal, even of no bytes.
  for (const VkDeviceSize size : {VkDeviceSize{64}, VkDeviceSize{0}}) {
    const Status empty = two.write_buffer(Buffer{}, 0, bytes.data(), size);
    EXPECT_TRUE(refused(empty, "empty")) << empty.message();
  }
  // CommandBatch's own refusal comes through.
  Result<Buffer> small = buffer(32);
  ASSERT_TRUE(small.ok());
  EXPECT_EQ(two.write_buffer(*small, 0, bytes.data(), 64).domain(),
            Status::Code::InvalidArgument);

  // Images, each refused before anything is recorded: the layout stays.
  const auto stays = [&](Image& target, VkDeviceSize size, const char* what) {
    const Status status =
        two.write_image(target, bytes.data(), size, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_TRUE(refused(status, what)) << status.message();
    EXPECT_EQ(target.layout(), VK_IMAGE_LAYOUT_UNDEFINED) << what;
  };
  Image empty;
  stays(empty, 64, "empty");
  Result<Image> exclusive =
      image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            /*exclusive=*/true);
  ASSERT_TRUE(exclusive.ok()) << exclusive.status().message();
  stays(*exclusive, 64, "EXCLUSIVE");
  // test::write_image's refusals come through.
  Result<Image> good = image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM);
  ASSERT_TRUE(good.ok()) << good.status().message();
  stays(*good, 63, "takes 64 bytes");
}

}  // namespace
}  // namespace volumetric_kit::core
