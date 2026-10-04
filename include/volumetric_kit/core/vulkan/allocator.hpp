// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file allocator.hpp
/// @brief The VMA allocator: device memory, and the factory for
///        @ref volumetric_kit::core::Buffer and
///        @ref volumetric_kit::core::Image.

#include <cstdint>
#include <memory>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Device;

/// @brief Where a resource's memory lives.
///
/// Every resource the GPU reads or writes directly is device-local, by
/// construction: kernel data is `DeviceOnly`, data the host writes for shaders
/// to read is `DeviceMapped`, and host memory -- `Staging` -- takes copy usage
/// alone, so the GPU reaches it only by a copy, never from a shader. On a
/// discrete GPU host memory sits across PCIe: recon measured a TSDF kernel at
/// 14.6 ms with its buffers there against 0.067 ms in VRAM (RTX 5090).
///
/// - `DeviceOnly`: on a discrete GPU (DRAM + VRAM), VRAM the host cannot
///   map; on unified memory, GPU-private memory where the device has it
///   (Apple), else the one pool, unmapped.
/// - `DeviceMapped`: on a discrete GPU, VRAM the host maps through the BAR
///   window; on unified memory, the one pool, mapped.
/// - `Staging`: on a discrete GPU, system RAM, mapped; on unified memory, the
///   one pool, mapped.
///
/// Each placement is a memory-type mask, not a preference, so it does not
/// depend on the order a driver lists its types, and a full heap fails the
/// allocation rather than move the resource somewhere slower.
/// @ref PhysicalDeviceInfo::unified_memory tells the two architectures apart.
enum class MemoryUsage {
  /// GPU memory the host never maps: `DEVICE_LOCAL` and not `HOST_VISIBLE`
  /// wherever the device has such a type for the resource -- a discrete
  /// GPU's VRAM outside its BAR window, Apple silicon's private storage. A
  /// device whose every device-local type is host-visible (lavapipe, most
  /// integrated and mobile GPUs) has one pool, and the resource lands there,
  /// unmapped. Never host memory, and never the BAR window. Kernel data, and
  /// every image.
  DeviceOnly,
  /// Device-local memory the host maps, coherently: a discrete GPU's BAR
  /// window -- all of VRAM under Resizable BAR, 256 MiB without it -- or the
  /// one pool of unified memory. For data the host writes and shaders read
  /// directly, as uniforms and per-frame parameters, and on unified memory
  /// for inputs with no staging copy. Write it with
  /// @ref HostAccess::SequentialWrite: on a discrete GPU the host's reads
  /// cross PCIe uncached, so results come back by copy. Never host memory:
  /// a device with no such type, or a full BAR window, fails the allocation.
  DeviceMapped,
  /// Host memory, mapped and coherent: system RAM on a discrete GPU --
  /// write-combined for @ref HostAccess::SequentialWrite uploads, cached for
  /// @ref HostAccess::Random readbacks -- never VRAM or the BAR window; the
  /// one pool on unified memory. A copy's source or destination only: its
  /// usage is `TRANSFER_SRC` and `TRANSFER_DST` at most.
  Staging,
};

/// @brief How the host touches a mapped buffer, which steers the memory type.
enum class HostAccess {
  /// Reads and writes in any order: cached host memory.
  Random,
  /// Writes only, front to back, never reads: lets VMA pick write-combined
  /// memory, which streams uploads faster on a discrete GPU. Reading through
  /// @ref Buffer::mapped after choosing it is undefined.
  SequentialWrite,
};

/// @brief One memory heap's usage and budget, in bytes.
///
/// VMA's running accounting, not a live driver query: heuristics unless the
/// device enables `VK_EXT_memory_budget`.
///
/// TODO: enable `VK_EXT_memory_budget` where offered (an optional extension
/// of the requirements) so these become the driver's own figures.
struct HeapStats {
  std::uint64_t usage_bytes = 0;   ///< Bytes VMA has allocated from the heap.
  std::uint64_t budget_bytes = 0;  ///< Bytes VMA estimates are usable.
};

/// @brief Per-heap usage and budget across a device's memory heaps.
///
/// A fixed array with a live count, so it is filled and returned without
/// touching the host heap. A unified-memory device (Apple silicon) typically
/// reports one heap where a discrete GPU reports separate device-local and
/// host heaps.
///
/// @code
/// const MemoryStats stats = allocator.memory_stats();
/// for (std::uint32_t h = 0; h < stats.heap_count; ++h) {
///   report(h, stats.heaps[h].usage_bytes, stats.heaps[h].budget_bytes);
/// }
/// @endcode
struct MemoryStats {
  std::uint32_t heap_count = 0;            ///< Valid entries in @ref heaps.
  HeapStats heaps[VK_MAX_MEMORY_HEAPS]{};  ///< Per-heap figures.
};

/// @brief Parameters for @ref Allocator::create_buffer.
///
/// TODO: V4 adds exportable memory here (gfx's `ExternalHandleType`), with the
/// rest of the external-memory interop.
struct BufferDesc {
  /// Size in bytes; non-zero.
  VkDeviceSize size = 0;
  /// Usage flags; non-zero, and without
  /// `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, which the allocator does not
  /// enable.
  VkBufferUsageFlags usage = 0;
  /// Where the memory lives; device-only unless said otherwise.
  MemoryUsage memory = MemoryUsage::DeviceOnly;
  /// Map the allocation persistently, reachable through @ref Buffer::mapped
  /// for the buffer's lifetime, and coherent, so writes need no flush.
  /// Required for `DeviceMapped` and `Staging`, refused for `DeviceOnly`.
  bool mapped = false;
  /// How the host touches the mapping; read only when @ref mapped.
  HostAccess host_access = HostAccess::Random;
  /// The queue families that will access the buffer, or null for one.
  ///
  /// Null gives `VK_SHARING_MODE_EXCLUSIVE`, owned by whichever family uses it
  /// first -- right for a library's own buffers, wrong for one another library
  /// reads directly: on Apple a compute library and a renderer are handed
  /// queues from different families, and reading an exclusive buffer from a
  /// family that does not own it is undefined, often with no symptom. List the
  /// families that touch it: two or more distinct indices give
  /// `VK_SHARING_MODE_CONCURRENT`, one gives exclusive. Duplicates count once,
  /// so passing both families unconditionally costs nothing where they are the
  /// same family. At most @ref kMaxQueueFamilies distinct entries. The array
  /// need only outlive the create call.
  const std::uint32_t* queue_families = nullptr;
  /// The length of @ref queue_families; zero when it is null.
  std::uint32_t queue_family_count = 0;

  /// The most distinct queue families one resource can be shared between: a
  /// compute family and a render family, with room for a third consumer.
  static constexpr std::uint32_t kMaxQueueFamilies = 4;
};

/// @brief Check the entry count of a config that carries its queue families in
///        a fixed array of @ref BufferDesc::kMaxQueueFamilies, so a count past
///        the array is refused at the config's create rather than read past its
///        end at the first allocation.
/// @param count   The config's entry count.
/// @param caller  Named in the message, e.g. `"MarchingCubes::create"`.
/// @return OK, or @ref Status::Code::InvalidArgument past the array.
VKC_VULKAN_API Status check_queue_family_count(std::uint32_t count,
                                               const char* caller);

/// @brief Parameters for @ref Allocator::create_image.
///
/// The defaults describe a single-mip, single-layer, single-sample 2D image
/// in device-local memory, with a default view. Set `type` and `depth` for a
/// 3D (volume) image, `array_layers` for an array, `cube` for a cubemap,
/// `mip_levels` for a mip chain, `samples` for multisampling.
struct ImageDesc {
  /// Width and height in texels; non-zero.
  VkExtent2D extent{};
  /// Depth in texels; more than 1 requires `VK_IMAGE_TYPE_3D`.
  std::uint32_t depth = 1;
  /// Texel format; not `VK_FORMAT_UNDEFINED`.
  VkFormat format = VK_FORMAT_UNDEFINED;
  /// Usage flags; non-zero.
  VkImageUsageFlags usage = 0;
  /// 1D, 2D or 3D.
  VkImageType type = VK_IMAGE_TYPE_2D;
  /// Mip levels; non-zero.
  std::uint32_t mip_levels = 1;
  /// Array layers; non-zero, and 1 for a 3D image.
  std::uint32_t array_layers = 1;
  /// A cubemap: a square, six-layer, single-sample 2D image created
  /// `CUBE_COMPATIBLE`, whose default view is a cube view. Faces are layers
  /// +X, -X, +Y, -Y, +Z, -Z.
  bool cube = false;
  /// Sample count; multisampling needs a single-mip, optimal-tiling 2D image.
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  /// Tiling.
  VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
  /// Where the memory lives: `DeviceOnly`, the one placement an image takes,
  /// which also lets a driver compress and tile it (Apple's private
  /// storage). An image has no host accessor -- read one back by copying it
  /// into a staging buffer.
  MemoryUsage memory = MemoryUsage::DeviceOnly;
  /// Create a default view over every mip and layer. Clear it for a
  /// transfer-only image, and for a multi-planar or 4:2:2 format (a
  /// decoder's picture), whose view needs a sampler Y'CbCr conversion.
  bool with_view = true;
  /// The queue families that will access the image, as
  /// @ref BufferDesc::queue_families.
  const std::uint32_t* queue_families = nullptr;
  /// The length of @ref queue_families; zero when it is null.
  std::uint32_t queue_family_count = 0;
};

/// @brief Owns a VMA allocator over a device, and makes the buffers and images
///        allocated from it.
///
/// Separate from @ref Device: a library sharing an adopted device still gets
/// an allocator of its own, as VMA allocators are independent bookkeeping
/// over the same `VkDevice` memory. Make one per library per device.
///
/// Thread-safe: VMA locks its own state, so several threads may allocate at
/// once.
///
/// Buffers and images need not be destroyed before the allocator: each holds a
/// reference to the VMA state, which is freed once the allocator and
/// everything made from it are gone. Structural rather than documented,
/// because a documented order cannot express `a = std::move(b)`, which ends a
/// resource's life while the wrapper `a` visibly lives on.
///
/// @warning The instance and @ref Device passed to @ref create must outlive the
///          allocator and every resource made from it.
///
/// @code
/// VKC_ASSIGN(Allocator allocator,
///            Allocator::create(instance.handle(), device));
/// BufferDesc staging;
/// staging.size = bytes;
/// staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
/// staging.memory = MemoryUsage::Staging;
/// staging.mapped = true;
/// staging.host_access = HostAccess::SequentialWrite;
/// VKC_ASSIGN(Buffer upload, allocator.create_buffer(staging));
/// std::memcpy(upload.mapped(), data, bytes);
/// @endcode
class VKC_VULKAN_API Allocator {
 public:
  /// @brief Create a VMA allocator over @p device.
  /// @param instance  The instance @p device belongs to.
  /// @param device    The device to allocate on; it must outlive this.
  /// @return The allocator; @ref Status::Code::InvalidArgument for a null
  ///         @p instance or an empty @p device; or a backend @ref Status.
  static Result<Allocator> create(VkInstance instance, const Device& device);

  ~Allocator();
  Allocator(Allocator&& other) noexcept;
  Allocator& operator=(Allocator&& other) noexcept;
  Allocator(const Allocator&) = delete;
  Allocator& operator=(const Allocator&) = delete;

  /// @brief Allocate a buffer and its memory.
  /// @param desc  Size, usage, memory, mapping and sharing.
  /// @return The buffer; @ref Status::Code::InvalidArgument for a zero size
  ///         or usage, a device-address usage, a mapped device-only buffer,
  ///         an unmapped device-mapped or staging one (there is no separate
  ///         map), a staging buffer with usage beyond the transfers, more
  ///         than @ref BufferDesc::kMaxQueueFamilies distinct families, or a
  ///         family the device does not have; @ref Status::Code::Unsupported
  ///         for device-mapped memory on a device without it; or a backend
  ///         @ref Status (`VK_ERROR_OUT_OF_DEVICE_MEMORY` when the heap is
  ///         full: nothing moves to slower memory).
  Result<Buffer> create_buffer(const BufferDesc& desc);

  /// @brief Allocate an image and its memory, and its default view.
  /// @param desc  Extent, format, usage, type, mips, layers, samples, tiling,
  ///              memory, view and sharing.
  /// @return The image; @ref Status::Code::InvalidArgument for a zero extent,
  ///         depth, mip or layer count, no usage, an undefined format, a depth
  ///         without a 3D type, a 1D image taller than 1, an arrayed 3D image,
  ///         a malformed cube, a multisampled image that is not single-mip
  ///         optimal 2D, a view of a transfer-only image or of a format that
  ///         needs a Y'CbCr conversion, memory other than device-only, or a
  ///         bad sharing list; or a backend @ref Status. The view spans every
  ///         mip and layer; its type follows the image (the
  ///         `_ARRAY` variant when arrayed, `CUBE` for a cube), and its aspect
  ///         the format (depth, stencil, or color).
  Result<Image> create_image(const ImageDesc& desc);

  /// @brief Per-heap usage and budget, for what this allocator allocated.
  ///
  /// A device shared between libraries gives each its own allocator, so each
  /// reports only its own share.
  /// @return The figures; `heap_count == 0` for a moved-from allocator.
  MemoryStats memory_stats() const;

  /// @return Whether this owns an allocator (`false` when moved-from).
  bool valid() const noexcept { return impl_ != nullptr; }

 private:
  Allocator() = default;

  // The VMA handle stays out of this header. Shared, because every buffer
  // and image holds a reference so it can free through it.
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace volumetric_kit::core
