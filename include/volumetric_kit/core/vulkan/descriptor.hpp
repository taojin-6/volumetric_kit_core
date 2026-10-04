// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file descriptor.hpp
/// @brief Descriptor set layouts, pools, and the sets allocated from them.

#include <cstdint>
#include <memory>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class DescriptorSet;

/// @brief Owns a `VkDescriptorSetLayout`: which bindings a set has, of which
///        type, in which stages.
///
/// @warning The device passed to @ref create must outlive the layout.
///
/// @code
/// VkDescriptorSetLayoutBinding voxels{};
/// voxels.binding = 0;
/// voxels.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
/// voxels.descriptorCount = 1;
/// voxels.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
/// VKC_ASSIGN(DescriptorSetLayout layout,
///            DescriptorSetLayout::create(device.handle(), &voxels, 1));
/// @endcode
class VKC_VULKAN_API DescriptorSetLayout {
 public:
  /// @brief Create a layout from @p count bindings.
  /// @param device    The device.
  /// @param bindings  The bindings; null only when @p count is 0.
  /// @param count     How many.
  /// @return The layout; @ref Status::Code::InvalidArgument for a null
  ///         @p device or null @p bindings with a count; or a backend
  ///         @ref Status.
  static Result<DescriptorSetLayout> create(
      VkDevice device, const VkDescriptorSetLayoutBinding* bindings,
      std::uint32_t count);

  /// @brief Construct an empty layout; @ref valid is false.
  DescriptorSetLayout() noexcept = default;
  DescriptorSetLayout(DescriptorSetLayout&&) noexcept = default;
  DescriptorSetLayout& operator=(DescriptorSetLayout&&) noexcept = default;
  DescriptorSetLayout(const DescriptorSetLayout&) = delete;
  DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;
  ~DescriptorSetLayout() = default;

  /// @return The layout (`VK_NULL_HANDLE` when empty).
  VkDescriptorSetLayout handle() const noexcept { return layout_.get(); }
  /// @return Whether this owns a layout.
  bool valid() const noexcept { return layout_.valid(); }

 private:
  UniqueHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> layout_;
};

/// @brief Owns a `VkDescriptorPool` and allocates @ref DescriptorSet s from it.
///
/// Its sets are freed with the pool, not one by one, so retire the pool only
/// once the GPU is done with every set drawn from it. A set, and every copy of
/// it, reads as empty once its pool is destroyed or replaced, so what recorded
/// it -- a @ref CommandBatch -- can tell it is gone.
///
/// @warning The device passed to @ref create must outlive the pool.
///
/// @code
/// const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
/// VKC_ASSIGN(DescriptorPool pool,
///            DescriptorPool::create(device.handle(), &size, 1, 1));
/// VKC_ASSIGN(DescriptorSet set, pool.allocate(layout.handle()));
/// @endcode
class VKC_VULKAN_API DescriptorPool {
 public:
  /// @brief Create a pool for @p max_sets sets drawing on @p sizes.
  /// @param device      The device.
  /// @param sizes       Per-type capacities; non-null.
  /// @param size_count  How many; non-zero.
  /// @param max_sets    How many sets it can hold; non-zero.
  /// @return The pool; @ref Status::Code::InvalidArgument for a null
  ///         @p device, empty @p sizes, or a zero @p max_sets; or a backend
  ///         @ref Status.
  static Result<DescriptorPool> create(VkDevice device,
                                       const VkDescriptorPoolSize* sizes,
                                       std::uint32_t size_count,
                                       std::uint32_t max_sets);

  /// @brief Construct an empty pool; @ref valid is false.
  DescriptorPool() noexcept = default;
  DescriptorPool(DescriptorPool&&) noexcept = default;
  DescriptorPool& operator=(DescriptorPool&&) noexcept = default;
  DescriptorPool(const DescriptorPool&) = delete;
  DescriptorPool& operator=(const DescriptorPool&) = delete;
  ~DescriptorPool() = default;

  /// @brief Allocate one set with @p layout's bindings. The pool owns it.
  /// @param layout  The set layout.
  /// @return The set; @ref Status::Code::InvalidArgument for an empty pool
  ///         or a null @p layout; or a backend @ref Status, e.g.
  ///         `VK_ERROR_OUT_OF_POOL_MEMORY` once the pool is exhausted.
  Result<DescriptorSet> allocate(VkDescriptorSetLayout layout);

  /// @return The pool (`VK_NULL_HANDLE` when empty).
  VkDescriptorPool handle() const noexcept { return pool_.get(); }
  /// @return Whether this owns a pool.
  bool valid() const noexcept { return pool_.valid(); }

 private:
  UniqueHandle<VkDescriptorPool, vkDestroyDescriptorPool> pool_;
  // Shared with nothing but watched by every set allocated here: it goes
  // with the pool, which frees the sets.
  std::shared_ptr<const void> lifetime_;
};

/// @brief A `VkDescriptorSet`, owned by its @ref DescriptorPool, and the writes
///        that bind resources into it.
///
/// Freely copyable: it borrows the set. Copies share a count of the writes made
/// through any of them, so a command batch that recorded the set can refuse it
/// once it has been rewritten through an alias -- or once its pool is gone, as
/// a set from @ref DescriptorPool::allocate, and every copy, then reads as
/// empty. As for the `VkDescriptorSet` itself, writes through any copy need
/// external synchronization, and every resource a write names must outlive the
/// work that reads it.
///
/// A write names a real buffer or view: a null one is valid only under the
/// `nullDescriptor` feature, which the tier does not enable, so it aborts via
/// @ref VKC_CHECK rather than reach the driver.
///
/// @code
/// set.write_storage_buffer(0, voxels.handle(), 0, VK_WHOLE_SIZE);
/// set.write_combined_image_sampler(1, color.view(), sampler,
///                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
/// @endcode
class VKC_VULKAN_API DescriptorSet {
 public:
  /// @brief Construct an empty set; @ref valid is false.
  DescriptorSet() noexcept = default;

  /// @brief Wrap a set allocated on @p device; @ref DescriptorPool::allocate
  ///        makes these. A set wrapped here cannot see its pool, so it stays
  ///        valid until the wrapper and its copies are gone.
  /// @param device  The device its writes update through.
  /// @param set     The set.
  DescriptorSet(VkDevice device, VkDescriptorSet set);

  /// @brief Bind a storage buffer at @p binding.
  /// @param binding  The `layout(binding = N)` slot.
  /// @param buffer   The buffer.
  /// @param offset   The byte offset into it.
  /// @param range    The bytes bound, or `VK_WHOLE_SIZE`.
  /// @pre @ref valid, and @p buffer non-null; otherwise aborts via
  ///      @ref VKC_CHECK.
  void write_storage_buffer(std::uint32_t binding, VkBuffer buffer,
                            VkDeviceSize offset, VkDeviceSize range) const;
  /// @brief Bind a uniform buffer at @p binding.
  /// @param binding  The slot.
  /// @param buffer   The buffer.
  /// @param offset   The byte offset into it.
  /// @param range    The bytes bound, or `VK_WHOLE_SIZE`.
  /// @pre @ref valid, and @p buffer non-null.
  void write_uniform_buffer(std::uint32_t binding, VkBuffer buffer,
                            VkDeviceSize offset, VkDeviceSize range) const;
  /// @brief Bind an image and its sampler (a GLSL `sampler2D`) at @p binding.
  /// @param binding  The slot.
  /// @param view     The image's view.
  /// @param sampler  The sampler; null when the layout binds an immutable one.
  /// @param layout   The layout the image is in when sampled, typically
  ///                 `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
  /// @pre @ref valid, and @p view non-null.
  void write_combined_image_sampler(std::uint32_t binding, VkImageView view,
                                    VkSampler sampler,
                                    VkImageLayout layout) const;
  /// @brief Bind a storage image (a GLSL `image2D`) at @p binding, as compute
  ///        kernels that write images take it.
  /// @param binding  The slot.
  /// @param view     The image's view.
  /// @param layout   The layout the image is in, `VK_IMAGE_LAYOUT_GENERAL`.
  /// @pre @ref valid, and @p view non-null (an image made `with_view`).
  void write_storage_image(std::uint32_t binding, VkImageView view,
                           VkImageLayout layout) const;

  /// @return The set (`VK_NULL_HANDLE` when empty, or once its pool is gone).
  VkDescriptorSet handle() const noexcept {
    if (state_ == nullptr || (state_->pooled && state_->pool.expired())) {
      return VK_NULL_HANDLE;
    }
    return state_->set;
  }
  /// @return Whether this refers to a set whose pool still holds it.
  bool valid() const noexcept { return handle() != VK_NULL_HANDLE; }
  /// @return How many writes this set and its copies have made.
  std::uint64_t writes() const noexcept {
    return state_ != nullptr ? state_->writes : 0;
  }

 private:
  // The one write the four share: checks the set and the resource, updates
  // one descriptor of `type` from `buffer` or `image` (the other null), and
  // counts it. `caller` names the public write in a failed check.
  void write(const char* caller, VkDescriptorType type, std::uint32_t binding,
             const VkDescriptorBufferInfo* buffer,
             const VkDescriptorImageInfo* image) const;

  friend class DescriptorPool;  // to tie the sets it allocates to it

  struct State {
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint64_t writes = 0;
    // The allocating pool's lifetime; `pooled` tells an expired one from a
    // wrapped set's, which has none to watch.
    std::weak_ptr<const void> pool;
    bool pooled = false;
  };
  std::shared_ptr<State> state_;
};

}  // namespace volumetric_kit::core
