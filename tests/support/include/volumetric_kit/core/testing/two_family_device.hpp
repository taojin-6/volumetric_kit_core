// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file two_family_device.hpp
/// @brief A headless @ref volumetric_kit::core::SharedDevice whose compute
///        library and renderer submit to two queue families, for the family's
///        cross-family tests, with no test framework
///        (`volumetric_kit::core_test_policy`).
///
/// A producer's buffer or image that another library reads in place must be
/// `CONCURRENT` across the two families, and the validation layer does not
/// report a read of an `EXCLUSIVE` one from the wrong family. A test of that
/// path needs the two families a product gets: @ref
/// volumetric_kit::core::test::TwoFamilyDevice makes them the way
/// `SharedDevice::create` carves them, so it crosses families exactly where a
/// product would, however many families the device has. A GoogleTest suite
/// takes it from `SharedDeviceTest` (`vulkan_fixture.hpp`), one per process.
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// // In a test that is a main() of its own, under its ValidationSession and
/// // LogCapture.
/// int cross_family_case() {
///   Result<std::unique_ptr<test::TwoFamilyDevice>> made =
///       test::TwoFamilyDevice::create(compute_requirements, {});
///   if (!made) return 1;
///   test::TwoFamilyDevice& two = **made;
///   if (!two.crosses_families()) {
///     std::printf("skipped: %s\n", two.shared().summary().c_str());
///     return 0;
///   }
///   BufferDesc desc;
///   desc.size = bytes;
///   desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
///                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
///   desc.queue_families = two.families().data();
///   desc.queue_family_count = test::TwoFamilyDevice::kFamilyCount;
///   Result<Buffer> shared = two.compute_allocator().create_buffer(desc);
///   if (!shared || !two.write_buffer(*shared, 0, data, bytes).ok()) return 1;
///   // ... read *shared on two.graphics().
///   return 0;
/// }
/// @endcode

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

/// @brief A headless shared device, adopted by a compute library and by a
///        renderer, with an allocator each, and writes on the compute
///        library's queue for the renderer to read.
///
/// The device and its queues are `SharedDevice::create`'s, which takes two
/// families only under @ref QueuePlan::TwoFamilies. On a device where it puts
/// both libraries on one family -- two queues of it, or one shared queue --
/// no product shares a resource across families there, so
/// @ref crosses_families is false and a cross-family test skips.
///
/// Pinned in memory (made only as a `unique_ptr`): @ref families is read
/// through a pointer. Everything made on its devices and allocators must be
/// destroyed before it.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<test::TwoFamilyDevice> two,
///            test::TwoFamilyDevice::create(recon_requirements,
///                                          renderer_requirements));
/// log(two->shared().summary());  // the plan, and each library's family
/// @endcode
class TwoFamilyDevice {
 public:
  /// The families @ref families names: the compute library's, then the
  /// renderer's.
  static constexpr std::uint32_t kFamilyCount = 2;

  /// @brief Make the shared device for both libraries, adopt it for each with
  ///        its requirements, and make each an allocator.
  ///
  /// The instance is @ref instance_config's at @p validation, made with that
  /// level's layer settings in the environment (@ref ValidationSession).
  /// @param compute     The compute library's requirements (recon's); its
  ///                    queue does compute whatever they say.
  /// @param graphics    The renderer's (gfx's); its queue does graphics
  ///                    whatever they say. Headless: not `needs_present`.
  /// @param validation  The level; the environment's by default.
  /// @return The device on any queue plan; @ref Status::Code::InvalidArgument
  ///         for requirements that present or cannot merge, as
  ///         `SharedDevice::create` says, or why no device meets them;
  ///         @ref check_layer_loaded's refusal of the instance; or an adopt's
  ///         or an allocator's failure.
  static Result<std::unique_ptr<TwoFamilyDevice>> create(
      const DeviceRequirements& compute, const DeviceRequirements& graphics,
      Validation validation = requested_validation());

  ~TwoFamilyDevice();
  TwoFamilyDevice(const TwoFamilyDevice&) = delete;
  TwoFamilyDevice& operator=(const TwoFamilyDevice&) = delete;
  TwoFamilyDevice(TwoFamilyDevice&&) = delete;
  TwoFamilyDevice& operator=(TwoFamilyDevice&&) = delete;

  /// @return Whether the two libraries submit to different queue families:
  ///         `SharedDevice::create` took @ref QueuePlan::TwoFamilies.
  bool crosses_families() const noexcept {
    return shared_->plan() == QueuePlan::TwoFamilies;
  }
  /// @return The compute library's queue family, then the renderer's, for
  ///         @ref BufferDesc::queue_families and @ref ImageDesc::queue_families
  ///         with @ref kFamilyCount: `CONCURRENT` when they differ.
  const std::array<std::uint32_t, kFamilyCount>& families() const noexcept {
    return families_;
  }

  /// @return The shared device: its instance, physical device, plan and
  ///         summary.
  const SharedDevice& shared() const noexcept { return *shared_; }
  /// @return The compute library's device, adopted with its requirements.
  Device& compute();
  /// @return The renderer's device, adopted with its requirements.
  Device& graphics();
  /// @return An allocator on @ref compute.
  Allocator& compute_allocator();
  /// @return An allocator on @ref graphics.
  Allocator& graphics_allocator();

  /// @brief Write @p bytes into @p dst at @p offset on the compute library's
  ///        queue, and wait for its fence: the writes are complete, and
  ///        available to a later submit on either queue.
  ///
  /// Work of the test's own, such as a kernel's dispatch, goes through
  /// `compute().submit_single_time`, which waits for its fence too.
  /// @param dst     Needs `TRANSFER_DST` usage, and is `CONCURRENT` where
  ///                @ref crosses_families.
  /// @param offset  The byte offset into @p dst.
  /// @param data    The bytes; null only when @p bytes is 0.
  /// @param bytes   How many.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty @p dst, an
  ///         `EXCLUSIVE` one across families, whose read on the renderer's
  ///         family would be undefined with nothing to report it, or for what
  ///         @ref CommandBatch::upload refuses; or a submit's failure.
  Status write_buffer(const Buffer& dst, VkDeviceSize offset, const void* data,
                      VkDeviceSize bytes);

  /// @brief @ref test::write_image on the compute library's queue and
  ///        allocator, for the renderer to read: mip 0, layer 0 written,
  ///        the whole image left in @p layout, the fence waited for.
  /// @param dst     As @ref test::write_image takes it, and `CONCURRENT`
  ///                where @ref crosses_families.
  /// @param texels  Width x height x depth texels, rows packed.
  /// @param bytes   Their size, as @ref test::write_image says.
  /// @param layout  The layout to leave it in, as @ref test::write_image
  ///                says.
  /// @return OK; @ref Status::Code::InvalidArgument for an `EXCLUSIVE` image
  ///         across families, or for what @ref test::write_image refuses; or
  ///         its staging allocation's or submit's failure.
  Status write_image(Image& dst, const void* texels, VkDeviceSize bytes,
                     VkImageLayout layout);

 private:
  explicit TwoFamilyDevice(std::unique_ptr<SharedDevice> shared);
  Status check_shared(VkSharingMode sharing, const char* call) const;

  // Declared first, so destroyed last: the adopters and allocators borrow it.
  std::unique_ptr<SharedDevice> shared_;
  std::array<std::uint32_t, kFamilyCount> families_{};
  std::optional<Device> compute_;
  std::optional<Device> graphics_;
  std::optional<Allocator> compute_allocator_;
  std::optional<Allocator> graphics_allocator_;
};

}  // namespace volumetric_kit::core::test
