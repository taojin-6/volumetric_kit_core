// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file external_memory.hpp
/// @brief Device memory another API on the same GPU writes into -- CUDA
///        writing a hardware decoder's picture, which the kernels then read
///        in place -- the descriptor that hands it across, and the
///        memory-type search for a resource bound outside the
///        @ref Allocator.

#include <cstdint>
#include <optional>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"  // IWYU pragma: export
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Allocator;
class Device;

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

/// @brief The memory type to bind a resource to that is not allocated through
///        the @ref Allocator: the first that @p type_bits allows with every
///        flag of @p required and none of @p excluded.
///
/// Protected, lazily allocated, and feature-gated device-coherent and
/// device-uncached types are never chosen: the features they need are not
/// enabled. Of the rest, Vulkan orders a type ahead of any whose flags strictly
/// contain its own, so the first match is the plainest.
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

}  // namespace volumetric_kit::core
