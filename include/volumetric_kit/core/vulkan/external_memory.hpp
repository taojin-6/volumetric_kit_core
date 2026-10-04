// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file external_memory.hpp
/// @brief Device memory another API on the same GPU writes into -- CUDA
///        writing a hardware decoder's picture, which the kernels then read
///        in place -- and the memory-type search for a resource bound outside
///        the @ref Allocator.

#include <cstdint>
#include <optional>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Device;

/// @brief A buffer on memory of its own, and a file descriptor for that
///        memory, for another API to import.
///
/// The descriptor is a plain `int`, as the importing API takes it: an import
/// that succeeds owns it (CUDA's does); until then the caller does, and
/// closes it -- after a failed import, too.
///
/// @code
/// VKC_ASSIGN(ExportedBuffer exported, create_exported_buffer(device, bytes));
/// cudaExternalMemoryHandleDesc desc{};
/// desc.type = cudaExternalMemoryHandleTypeOpaqueFd;
/// desc.handle.fd = exported.fd;
/// desc.size = exported.memory_size;
/// if (cudaImportExternalMemory(&memory, &desc) != cudaSuccess) {
///   close(exported.fd);  // a failed import leaves it ours
/// }
/// @endcode
struct ExportedBuffer {
  /// A device-local storage buffer, with transfer usage, on dedicated memory
  /// it frees with itself.
  Buffer buffer;
  /// An opaque file descriptor for the memory (`VK_KHR_external_memory_fd`).
  int fd = -1;
  /// The memory's size, which an importer is told; at least the buffer's.
  VkDeviceSize memory_size = 0;
};

/// @brief The memory type to bind a resource to that is not allocated through
///        the @ref Allocator: the first that @p type_bits allows with every
///        flag of @p required and none of @p excluded.
///
/// Vulkan orders a type ahead of any whose flags strictly contain its own, so
/// the first match is the plainest: asked for device-local alone, it is one
/// the host cannot map wherever @p type_bits allows one, keeping the resource
/// out of a discrete GPU's BAR window. Protected, lazily allocated and AMD
/// device-coherent types are never chosen: the features they need are not
/// enabled.
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
/// @return The type's index, or empty when no allowed type fits.
VKC_VULKAN_API std::optional<std::uint32_t> find_memory_type(
    const Device& device, std::uint32_t type_bits,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded = 0);

/// @brief Make a buffer that another API on the same GPU imports and writes.
///
/// Its memory comes from Vulkan directly, not the @ref Allocator: exported
/// memory is dedicated to its resource, and the few a decoder keeps need no
/// pool. It is placed as @ref MemoryUsage::DeviceOnly is: device memory the
/// host cannot map wherever the buffer allows it, else device-local memory --
/// never host memory. Before a kernel reads what the importer wrote, a
/// @ref CommandBatch takes it over from `VK_QUEUE_FAMILY_EXTERNAL`
/// (@ref CommandBatch::acquire).
///
/// TODO: Windows handles (`VK_KHR_external_memory_win32`) when a sibling
/// builds there; the family's CUDA interop is Linux.
///
/// @code
/// VKC_ASSIGN(ExportedBuffer frame, create_exported_buffer(device, bytes));
/// // ... CUDA imports frame.fd and writes the decoded picture ...
/// CommandBatch batch(device, allocator);
/// VKC_TRY(batch.acquire(frame.buffer, VK_QUEUE_FAMILY_EXTERNAL));
/// VKC_TRY(batch.dispatch(convert, &push, sizeof(push), groups, max_groups));
/// VKC_TRY(batch.submit());
/// @endcode
/// @param device  A device that exports memory (@ref Device::exports_memory:
///                created with `VK_KHR_external_memory_fd` among its
///                extensions or optional extensions).
/// @param bytes   The buffer's size; not 0.
/// @return The buffer and its descriptor; @ref Status::Code::Unsupported
///         where @p device does not export memory, cannot export such a
///         buffer, or has no device-local memory it can use;
///         @ref Status::Code::InvalidArgument for 0 bytes; or a backend
///         @ref Status.
VKC_VULKAN_API Result<ExportedBuffer> create_exported_buffer(
    const Device& device, VkDeviceSize bytes);

}  // namespace volumetric_kit::core
