// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file buffer.hpp
/// @brief A `VkBuffer` and its backing allocation, owned and freed together,
///        and the memory-type record buffers and images share.

#include <cstdint>
#include <functional>
#include <optional>

#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief The memory type actually backing a buffer or image, read at
///        allocation.
///
/// Indices refer to the allocating physical device's memory properties.
/// `HOST_VISIBLE` and `DEVICE_LOCAL` can both be set -- on unified memory
/// (Apple silicon, mobile) and on a discrete GPU's host-visible device heap --
/// so being mappable does not mean living in system memory.
///
/// @code
/// const bool in_vram = buffer.memory_info() &&
///                      (buffer.memory_info()->properties &
///                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
/// @endcode
struct MemoryInfo {
  VkMemoryPropertyFlags properties = 0;  ///< The type's property flags.
  std::uint32_t type_index = 0;          ///< The Vulkan memory type.
  std::uint32_t heap_index = 0;          ///< The heap backing that type.
};

/// @brief Owns a `VkBuffer` and the allocation backing it, freeing both
///        together.
///
/// Made by @ref Allocator::create_buffer, or adopted from someone else's
/// allocation. The allocation lives inside a type-erased deleter, so VMA never
/// reaches this header. A buffer made `BufferDesc::mapped` exposes a
/// persistent, host-coherent pointer through @ref mapped; one made
/// `MemoryUsage::DeviceLocal` is never mapped.
///
/// The buffer records what Vulkan cannot be asked afterwards -- its usage,
/// sharing mode and memory type -- so a library handed a borrowed buffer can
/// check it permits the binding it is about to make, may be read from its
/// queue family, and lives where its kernels need it.
///
/// A buffer from an @ref Allocator keeps that allocator's VMA state alive
/// until it is freed, so it may outlive the `Allocator` object; the device
/// must still outlive it.
///
/// @code
/// BufferDesc desc;
/// desc.size = 1 << 20;
/// desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
/// desc.memory = MemoryUsage::DeviceLocal;
/// VKC_ASSIGN(Buffer voxels, allocator.create_buffer(desc));
/// @endcode
class VKC_VULKAN_API Buffer {
 public:
  /// @brief Construct an empty buffer; @ref valid is false.
  Buffer() noexcept = default;

  /// @brief Adopt @p handle and its allocation, freed by @p deleter.
  /// @param handle   The `VkBuffer`.
  /// @param size     Its size in bytes.
  /// @param usage    The `VkBufferUsageFlags` it was created with.
  /// @param sharing  The `VkSharingMode` it was created with.
  /// @param mapped   Its persistent host pointer, or null when unmapped.
  /// @param deleter  Frees the buffer and its allocation, exactly once.
  /// @param memory   The memory type backing it, or empty when unknown. An
  ///                 adopter that supplies it must read the bound
  ///                 allocation's type, not infer it from usage; a buffer
  ///                 with unknown memory is refused where device-local memory
  ///                 is required.
  Buffer(VkBuffer handle, VkDeviceSize size, VkBufferUsageFlags usage,
         VkSharingMode sharing, void* mapped, std::function<void()> deleter,
         std::optional<MemoryInfo> memory) noexcept;

  ~Buffer();
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  /// @return The buffer (`VK_NULL_HANDLE` when empty).
  VkBuffer handle() const noexcept { return buffer_; }
  /// @return Its size in bytes (`0` when empty).
  VkDeviceSize size() const noexcept { return size_; }
  /// @return The usage flags it was created with (`0` when empty).
  VkBufferUsageFlags usage() const noexcept { return usage_; }
  /// @return The sharing mode it was created with
  ///         (`VK_SHARING_MODE_EXCLUSIVE` when empty). Reading an exclusive
  ///         buffer from a queue family that does not own it is undefined, so
  ///         a consumer on another family checks this before reading.
  VkSharingMode sharing_mode() const noexcept { return sharing_; }
  /// @return The persistent host pointer, or null when not mapped.
  void* mapped() const noexcept { return mapped_; }
  /// @return The memory type backing it; empty for an empty buffer or an
  ///         adopted one whose type was not supplied.
  const std::optional<MemoryInfo>& memory_info() const noexcept {
    return memory_;
  }
  /// @return Whether the known backing memory is `DEVICE_LOCAL` (possibly
  ///         `HOST_VISIBLE` too); `false` when unknown.
  bool is_device_local() const noexcept {
    return memory_.has_value() &&
           (memory_->properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
  }
  /// @return Whether this owns a buffer.
  bool valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }

 private:
  void destroy() noexcept;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceSize size_ = 0;
  VkBufferUsageFlags usage_ = 0;
  VkSharingMode sharing_ = VK_SHARING_MODE_EXCLUSIVE;
  void* mapped_ = nullptr;
  std::optional<MemoryInfo> memory_;
  std::function<void()> deleter_;
};

}  // namespace volumetric_kit::core
