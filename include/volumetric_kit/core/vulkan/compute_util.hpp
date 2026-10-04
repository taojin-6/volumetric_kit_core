// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file compute_util.hpp
/// @brief Host-side helpers every compute library repeats: the workgroup
///        count, the storage-binding range limit, storage-buffer creation --
///        device-only for the kernels, device-mapped for what the host writes
///        for them -- and an input that is either host bytes or a buffer
///        already on the device.
///
/// The mechanism lives here because every compute library repeats its shape;
/// the policy -- which buffers, which bindings -- stays in the library.

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class CommandBatch;
class Device;

/// @brief Workgroups for a 1-D dispatch of @p items threads at @p local_size
///        threads a group: `ceil(items / local_size)`.
///
/// Computed as `items / local_size + (items % local_size != 0)`, not the
/// `(items + local_size - 1) / local_size` idiom, which wraps for @p items
/// near `UINT32_MAX` to almost no groups -- a silent no-op dispatch.
///
/// @code
/// const std::uint32_t groups = group_count(voxel_count, 64);
/// @endcode
/// @param items       Threads, one per work item.
/// @param local_size  Threads a workgroup (the shader's `local_size_x`);
///                    non-zero.
/// @return The workgroups to dispatch.
inline std::uint32_t group_count(std::uint32_t items,
                                 std::uint32_t local_size) noexcept {
  return (items / local_size) + (items % local_size != 0U ? 1U : 0U);
}

/// @brief The device's `maxStorageBufferRange`: the most one storage-buffer
///        binding may cover, a separate limit from how much memory can be
///        allocated. Read once at a library's create and kept, as the
///        workgroup-count limit beside it is.
///
/// @code
/// max_range_ = max_storage_buffer_range(device);
/// @endcode
/// @param device  The device; its @ref Device::caps hold the limit.
/// @return The limit in bytes; 0 for a moved-from device.
VKC_VULKAN_API VkDeviceSize max_storage_buffer_range(const Device& device);

/// @brief Refuse a storage-buffer binding whose range the device does not
///        permit.
///
/// Binding more than `maxStorageBufferRange` -- a larger buffer bound with
/// `VK_WHOLE_SIZE` included -- is invalid usage: a validation-layer message,
/// and undefined with layers off, as they are where an app ships. The limit is
/// low enough to reach: Vulkan guarantees only 2^27 bytes (128 MiB), which
/// Android-class drivers report, while desktop drivers and MoltenVK report far
/// more -- so an oversized binding is invisible where it is tested and fatal
/// where it ships.
/// @param what       Names the caller and the buffer, for the message.
/// @param bytes      The range the binding would cover.
/// @param max_range  The limit, from @ref max_storage_buffer_range.
/// @return OK when @p bytes fits; @ref Status::Code::InvalidArgument
///         otherwise.
VKC_VULKAN_API Status check_storage_buffer_range(const char* what,
                                                 VkDeviceSize bytes,
                                                 VkDeviceSize max_range);

/// @brief A device-mapped storage buffer of @p bytes
///        (@ref MemoryUsage::DeviceMapped): device-local memory the host
///        writes and the kernels read in place -- parameters, small tables,
///        and on unified memory inputs with no staging copy.
///
/// The kernels reach it at device-local speed on every platform; on a
/// discrete GPU it is VRAM through the BAR window, which the host writes
/// across PCIe. Memory only the kernels touch is a @ref device_storage_buffer;
/// results the host reads come back by @ref CommandBatch::readback, as the
/// host's reads of the BAR window cross PCIe uncached.
/// @param allocator           The allocator.
/// @param bytes               Its size; non-zero.
/// @param access              How the host touches it:
///                            @ref HostAccess::SequentialWrite for what it
///                            writes once, the default; @ref HostAccess::Random
///                            only on unified memory, where reading it back is
///                            cheap.
/// @param extra_usage         Usage beyond `STORAGE_BUFFER`, for a consumer
///                            that binds the allocation another way (a
///                            renderer reading it as vertices).
/// @param queue_families      The families that access it, as
///                            @ref BufferDesc::queue_families: null leaves it
///                            exclusive, right for a library's own kernels; a
///                            buffer another library reads names both.
/// @param queue_family_count  The length of @p queue_families.
/// @return The buffer; @ref Status::Code::Unsupported on a device with no
///         device-local memory the host can map; or the allocation's
///         failure.
VKC_VULKAN_API Result<Buffer> mapped_storage_buffer(
    Allocator& allocator, VkDeviceSize bytes,
    HostAccess access = HostAccess::SequentialWrite,
    VkBufferUsageFlags extra_usage = 0,
    const std::uint32_t* queue_families = nullptr,
    std::uint32_t queue_family_count = 0);

/// @brief A @ref mapped_storage_buffer of @p bytes, filled from @p src: a
///        device-local input the kernels read in place, with no copy on the
///        device.
/// @param allocator  The allocator.
/// @param src        At least @p bytes to copy in; non-null.
/// @param bytes      Its size; non-zero.
/// @param access     How the host touches it; an input written once by
///                   default.
/// @return The filled buffer; @ref Status::Code::InvalidArgument for a null
///         @p src; or as @ref mapped_storage_buffer.
VKC_VULKAN_API Result<Buffer> upload_storage_buffer(
    Allocator& allocator, const void* src, VkDeviceSize bytes,
    HostAccess access = HostAccess::SequentialWrite);

/// @brief A device-only storage buffer of @p bytes
///        (@ref MemoryUsage::DeviceOnly), for memory only the kernels touch.
///
/// On a discrete GPU it is VRAM the host cannot map, so nothing it holds
/// crosses PCIe but what a @ref CommandBatch copies; memory that is not
/// device-local would be reached across PCIe at every access.
/// recon measured a hash table's bucket locks there at 1.97 s to allocate a
/// 5 000-triangle sheet, against 3.4 ms device-local, and a TSDF integrate at
/// 14.6 ms against 0.067 ms (RTX 5090). `TRANSFER_SRC` and `TRANSFER_DST`
/// come with it, so a @ref CommandBatch can fill, copy, upload into and read
/// back from it.
/// @param allocator           The allocator.
/// @param bytes               Its size; non-zero.
/// @param extra_usage         Usage beyond those, for a consumer that binds
///                            the allocation another way (a renderer reading
///                            it as vertices).
/// @param queue_families      The families that access it, as
///                            @ref BufferDesc::queue_families: null leaves it
///                            exclusive, right for a library's own kernels; a
///                            buffer another library reads names both.
/// @param queue_family_count  The length of @p queue_families.
/// @return The buffer, unmapped, or the allocation's failure.
VKC_VULKAN_API Result<Buffer> device_storage_buffer(
    Allocator& allocator, VkDeviceSize bytes,
    VkBufferUsageFlags extra_usage = 0,
    const std::uint32_t* queue_families = nullptr,
    std::uint32_t queue_family_count = 0);

/// @brief Make @p buffer, retained device-local scratch, hold at least
///        @p bytes.
///
/// A buffer that fits is kept. Otherwise the replacement takes 1.5x headroom
/// (within @p max_range), so an input that creeps up does not reallocate on
/// every call, and the old one goes first: freed before the replacement is
/// made, so a grow never holds both -- or, given @p batch, kept by it until
/// it has run. Contents are not kept.
///
/// So call it before recording what uses @p buffer, as a call does at its
/// start; a grow after that must pass the batch that recorded it. No other
/// batch not yet submitted may use @p buffer.
///
/// @code
/// VKC_TRY(ensure_device_scratch(device, allocator, counts_, count_bytes,
///                               max_range_, "codec.counts"));
/// @endcode
/// @param device     Names the buffer for a GPU capture.
/// @param allocator  The allocator.
/// @param buffer     The retained buffer; empty after a failed grow.
/// @param bytes      The range the next binding covers; 0 keeps the buffer.
/// @param max_range  The limit, from @ref max_storage_buffer_range.
/// @param name       The debug name, `library.buffer`, which also labels the
///                   range error.
/// @param batch      Optional; a batch whose recorded commands use @p buffer,
///                   which a grow hands the old one to
///                   (@ref CommandBatch::retain).
/// @return OK; @ref Status::Code::InvalidArgument when @p bytes exceeds
///         @p max_range; or the allocation's failure.
VKC_VULKAN_API Status ensure_device_scratch(const Device& device,
                                            Allocator& allocator,
                                            Buffer& buffer, VkDeviceSize bytes,
                                            VkDeviceSize max_range,
                                            const char* name,
                                            CommandBatch* batch = nullptr);

/// @brief An input a call reads as a storage binding: host bytes the call
///        stages onto the device, or a storage buffer already there (another
///        pass's output), bound in place. One or the other, by construction.
///
/// So a call takes both with one check and one binding path. Both are
/// borrowed, and outlive the call.
///
/// @code
/// Status integrate(const StorageInput& depth, ...) {
///   VKC_TRY(depth.check("integrate: depth", depth_bytes));
///   CommandBatch batch(device, allocator);
///   VKC_ASSIGN(VkBuffer bound,
///              depth.buffer(batch, allocator, depth_bytes, depth_upload_));
///   kernel.set.write_storage_buffer(0, bound, 0, depth_bytes);
///   ...
/// }
/// @endcode
class VKC_VULKAN_API StorageInput {
 public:
  /// @brief An input of host bytes, which @ref buffer stages.
  /// @param host  Host bytes; null is refused by @ref check.
  explicit StorageInput(const void* host) noexcept : host_(host) {}
  /// @brief An input already on the device, which @ref buffer binds in place.
  /// @param device  A storage buffer in known device-local memory, bound in
  ///                place: device-only, or device-mapped (the zero-copy
  ///                input of unified memory).
  explicit StorageInput(const Buffer& device) noexcept : device_(&device) {}
  /// A @ref Buffer is passed by reference: through a pointer it would be
  /// taken as host bytes, and the object itself uploaded.
  explicit StorageInput(const Buffer* device) = delete;

  /// @brief Whether this can be bound as @p bytes of storage. Constant time,
  ///        so a call checks it before doing any work.
  /// @param what   Names the caller and the input, for the message.
  /// @param bytes  What the binding reads.
  /// @return OK; @ref Status::Code::InvalidArgument for null host bytes, or
  ///         for a buffer that is empty, lacks `STORAGE_BUFFER` usage, holds
  ///         fewer than @p bytes (read past its end, undefined rather than an
  ///         error), or has unknown or non-device-local memory.
  Status check(const char* what, VkDeviceSize bytes) const;

  /// @brief The buffer to bind for @p bytes of this: the device buffer, or a
  ///        device-local buffer in @p upload that @p batch fills from the
  ///        host bytes.
  ///
  /// Bind exactly @p bytes of it, never `VK_WHOLE_SIZE`: a caller's buffer
  /// may exceed `maxStorageBufferRange` where the input does not. The caller
  /// keeps @p upload alive until the batch has run, and records the dispatch
  /// that reads it after this. An @p upload too small is replaced, and the
  /// old one handed to @p batch (@ref CommandBatch::retain), so commands it
  /// recorded on the old one still run on it; no other batch not yet
  /// submitted may use @p upload.
  /// @param batch      Records the upload.
  /// @param allocator  Makes the device buffer.
  /// @param bytes      The binding's range; non-zero, and checked by
  ///                   @ref check.
  /// @param upload     Receives the device buffer, reused when it holds
  ///                   @p bytes already, so a member kept across calls only
  ///                   grows; untouched for a device input.
  /// @return The handle to bind; @ref Status::Code::InvalidArgument for an
  ///         input @ref check refuses; or the allocation's or the upload's
  ///         failure.
  Result<VkBuffer> buffer(CommandBatch& batch, Allocator& allocator,
                          VkDeviceSize bytes, Buffer& upload) const;

 private:
  const void* host_ = nullptr;
  const Buffer* device_ = nullptr;
};

}  // namespace volumetric_kit::core
