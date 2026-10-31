// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file external_memory.hpp
/// @brief Device memory another API on the same GPU writes into -- CUDA
///        writing a hardware decoder's picture, which the kernels then read
///        or sample in place -- as a buffer or an image, the descriptor that
///        hands it across, and the memory-type search for a resource bound
///        outside the @ref Allocator.

#include <cstdint>
#include <optional>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"  // IWYU pragma: export
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Allocator;
class Device;
struct ImageDesc;

/// @brief A buffer on memory of its own, and a file descriptor for that
///        memory, for another API to import.
///
/// The memory is dedicated to the buffer, so an importer is told so -- CUDA
/// by `cudaExternalMemoryDedicated` -- and maps it as one resource.
///
/// @code
/// VKC_ASSIGN(ExportedBuffer exported,
///            create_exported_buffer(device, allocator, bytes));
/// cudaExternalMemoryHandleDesc desc{};
/// desc.type = cudaExternalMemoryHandleTypeOpaqueFd;
/// desc.handle.fd = exported.fd.get();
/// desc.size = exported.memory_size;
/// desc.flags = cudaExternalMemoryDedicated;
/// if (cudaImportExternalMemory(&memory, &desc) == cudaSuccess) {
///   exported.fd.release();  // CUDA owns it now; a failed import leaves it
/// }                         // here, closed with the struct
/// @endcode
struct ExportedBuffer {
  /// A device-local storage buffer, with transfer usage, on dedicated memory
  /// it frees with itself.
  Buffer buffer;
  /// An opaque file descriptor for the memory (`VK_KHR_external_memory_fd`).
  /// While open it keeps the memory alive after the buffer frees it, so it
  /// is released only to an import that takes it.
  UniqueFd fd;
  /// The memory's size, which an importer is told; at least the buffer's.
  VkDeviceSize memory_size = 0;
};

/// @brief An image on memory of its own, and a file descriptor for that
///        memory, for another API to import.
///
/// The memory is dedicated to the image, which starts at its offset 0, so an
/// importer is told so -- CUDA by `CUDA_EXTERNAL_MEMORY_DEDICATED` -- and
/// maps it whole as the image: CUDA as an array of the image's format and
/// extent, one level.
///
/// @code
/// VKC_ASSIGN(ExportedImage luma,
///            create_exported_image(device, allocator, desc));  // R8_UNORM
/// CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle{};
/// handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
/// handle.handle.fd = luma.fd.get();
/// handle.size = luma.memory_size;
/// handle.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
/// if (cuImportExternalMemory(&memory, &handle) == CUDA_SUCCESS) {
///   luma.fd.release();  // CUDA owns it now
/// }
/// CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC array{};
/// array.offset = 0;
/// array.arrayDesc = {luma.image.width(), luma.image.height(), 0,
///                    CU_AD_FORMAT_UNSIGNED_INT8, 1, 0};
/// array.numLevels = 1;
/// cuExternalMemoryGetMappedMipmappedArray(&levels, memory, &array);
/// @endcode
struct ExportedImage {
  /// A device-local 2D image with optimal tiling, on dedicated memory it
  /// frees with itself, and its default view unless the description cleared
  /// it. Its contents start undefined (`VK_IMAGE_LAYOUT_UNDEFINED`).
  Image image;
  /// An opaque file descriptor for the memory, owned as
  /// @ref ExportedBuffer::fd is.
  UniqueFd fd;
  /// The memory's size, which an importer is told; at least the image's.
  VkDeviceSize memory_size = 0;
};

/// @brief The memory type to bind a resource to that is not allocated through
///        the @ref Allocator: the first that @p type_bits allows with every
///        flag of @p required and none of @p excluded.
///
/// Protected and feature-gated device-coherent and device-uncached types are
/// never chosen, as the features they need are not enabled, and neither are
/// lazily allocated ones, which the @ref Allocator keeps for transient
/// attachments (@ref ImageDesc::usage). Of the rest, Vulkan orders a type
/// ahead of any whose flags strictly contain its own, so the first match is
/// the plainest.
///
/// Device-local memory asked for without `HOST_VISIBLE` is placed as
/// @ref MemoryUsage::DeviceOnly is: a type the host cannot map where the
/// device has one. On a discrete GPU, a resource that @p type_bits limits to
/// host-visible device-local types -- the BAR window -- gets none. On
/// unified memory with no such type, or none the resource allows, it is a
/// host-visible one of the one pool; exclude `HOST_VISIBLE` to refuse that
/// instead.
///
/// @code
/// VkMemoryRequirements needs{};
/// vkGetImageMemoryRequirements(device.handle(), image, &needs);
/// const std::optional<std::uint32_t> type = find_memory_type(
///     device, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
/// @endcode
/// @param device     The device whose memory types are searched.
/// @param type_bits  `VkMemoryRequirements::memoryTypeBits` of the resource.
/// @param required   Flags the type must have; 0 for any.
/// @param excluded   Flags the type must not have; 0 for none.
/// @return The type's index, or empty when no allowed type fits -- or, for
///         device-local memory without `HOST_VISIBLE`, when only a discrete
///         GPU's BAR window would.
VKC_VULKAN_API std::optional<std::uint32_t> find_memory_type(
    const Device& device, std::uint32_t type_bits,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded = 0);

/// @brief Make a buffer that another API on the same GPU imports and writes.
///
/// Its memory comes from @p allocator, dedicated to it, and is placed as
/// @ref MemoryUsage::DeviceOnly is -- never host memory, and on a discrete
/// GPU never the BAR window -- within its heap's budget, which it then counts
/// against as every allocation does.
/// Host bookkeeping failures return `OutOfMemory` and free any partial
/// buffer and allocation, leaving @p allocator usable for retry.
///
/// The buffer crosses between the APIs twice a frame, and the host orders
/// both crossings. The importer's writes must be done before the batch that
/// reads them is submitted -- for CUDA, a `cudaStreamSynchronize` of the
/// stream that wrote -- as the batch waits on no semaphore. The batch takes
/// the buffer over from `VK_QUEUE_FAMILY_EXTERNAL` before its kernels read
/// it and hands it back after them (@ref CommandBatch::acquire,
/// @ref CommandBatch::release), and the importer may write again once
/// @ref CommandBatch::submit has returned.
///
/// TODO: external semaphores (`VK_KHR_external_semaphore_fd`), so the two
/// APIs are ordered on the GPU rather than by the host waiting on each.
///
/// @code
/// VKC_ASSIGN(ExportedBuffer frame,
///            create_exported_buffer(device, allocator, bytes));
/// // ... CUDA imports frame.fd, writes the decoded picture, and its stream
/// // is synchronized ...
/// CommandBatch batch(device, allocator);
/// VKC_TRY(batch.acquire(frame.buffer, VK_QUEUE_FAMILY_EXTERNAL));
/// VKC_TRY(batch.dispatch(convert, &push, sizeof(push), groups, max_groups));
/// VKC_TRY(batch.release(frame.buffer, VK_QUEUE_FAMILY_EXTERNAL));
/// VKC_TRY(batch.submit());  // CUDA may write the next picture now
/// @endcode
/// @param device     A device that exports memory
///                   (@ref Device::exports_memory: created with
///                   `VK_KHR_external_memory_fd` among its extensions or
///                   optional extensions).
/// @param allocator  An allocator over @p device; the buffer frees its memory
///                   through it, as @ref Allocator::create_buffer's do.
/// @param bytes      The buffer's size; not 0.
/// @return The buffer and its descriptor; @ref Status::Code::Unsupported
///         where @p device does not export memory or cannot export such a
///         buffer, or no device-only memory type suits the buffer;
///         @ref Status::Code::InvalidArgument for 0 bytes, or a moved-from
///         @p allocator or one made for another device; or a backend
///         @ref Status
///         (`VK_ERROR_OUT_OF_DEVICE_MEMORY` when the heap is full or past
///         its budget).
VKC_VULKAN_API Result<ExportedBuffer> create_exported_buffer(
    const Device& device, Allocator& allocator, VkDeviceSize bytes);

/// @brief Make a single-plane 2D image that another API on the same GPU
///        imports and writes, for shaders to sample where it is: one plane of
///        a hardware decoder's picture, say, with no copy into an image of
///        Vulkan's own.
///
/// @p desc is made as @ref Allocator::create_image makes it -- its view and
/// its queue families included -- on memory placed, budgeted and dedicated
/// as @ref create_exported_buffer's is. A picture of several planes is an
/// image per plane. Host bookkeeping failures return `OutOfMemory` and free
/// any partial image, view and allocation, leaving @p allocator usable for
/// retry.
///
/// The image crosses between the APIs as an exported buffer does, its layout
/// with it. Before the importer's first write a batch hands the image over
/// (@ref CommandBatch::release to `VK_QUEUE_FAMILY_EXTERNAL`, in the layout
/// the importer expects: `GENERAL` for CUDA). After each write a batch takes
/// it back before reading it, and hands it over again after.
///
/// @code
/// ImageDesc desc;
/// desc.extent = {width, height};
/// desc.format = VK_FORMAT_R8_UNORM;
/// desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
/// VKC_ASSIGN(ExportedImage luma,
///            create_exported_image(device, allocator, desc));
/// CommandBatch first(device, allocator);
/// VKC_TRY(first.release(luma.image, VK_QUEUE_FAMILY_EXTERNAL,
///                       VK_IMAGE_LAYOUT_GENERAL));
/// VKC_TRY(first.submit());
/// luma.image.set_layout(VK_IMAGE_LAYOUT_GENERAL);
/// // ... each picture: CUDA writes it, and its stream is synchronized ...
/// CommandBatch batch(device, allocator);
/// VKC_TRY(batch.acquire(luma.image, VK_QUEUE_FAMILY_EXTERNAL,
///                       VK_IMAGE_LAYOUT_GENERAL));
/// VKC_TRY(batch.dispatch(convert, &push, sizeof(push), groups, max_groups));
/// VKC_TRY(batch.release(luma.image, VK_QUEUE_FAMILY_EXTERNAL,
///                       VK_IMAGE_LAYOUT_GENERAL));
/// VKC_TRY(batch.submit());  // CUDA may write the next picture now
/// @endcode
/// @param device     A device that exports memory, as
///                   @ref create_exported_buffer's.
/// @param allocator  An allocator over @p device; the image frees its memory
///                   through it.
/// @param desc       The image: 2D, one mip, one layer, one sample, optimal
///                   tiling, and a single-plane format, which is neither
///                   multi-planar nor 4:2:2.
/// @return The image and its descriptor; @ref Status::Code::Unsupported
///         where @p device does not export memory or cannot export an image
///         of this format, usage and extent, or no device-only memory type
///         suits the image; @ref Status::Code::InvalidArgument for a
///         moved-from @p allocator or one made for another device, a @p desc
///         that @ref Allocator::create_image refuses, or one that is not the
///         image above; or a backend @ref Status
///         (`VK_ERROR_OUT_OF_DEVICE_MEMORY` when the heap is full or past
///         its budget).
VKC_VULKAN_API Result<ExportedImage> create_exported_image(
    const Device& device, Allocator& allocator, const ImageDesc& desc);

}  // namespace volumetric_kit::core
