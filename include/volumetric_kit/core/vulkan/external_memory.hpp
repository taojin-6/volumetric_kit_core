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
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Allocator;
class Device;

/// @brief Sole owner of a file descriptor, closing it exactly once unless it
///        is released to the API that imports it.
///
/// An opaque descriptor keeps its memory alive after Vulkan frees it, so one
/// leaked on an early return would pin a whole dedicated allocation for the
/// life of the process. Owned, it is closed on every path until an import
/// takes it: an import that succeeds owns the descriptor (CUDA's does), so
/// the caller then calls @ref release; after a failed one it is still owned
/// here.
///
/// @code
/// UniqueFd fd(raw);                     // adopts
/// if (import(fd.get())) fd.release();  // the importer owns it now
/// @endcode
class VKC_VULKAN_API UniqueFd {
 public:
  /// @brief Construct an empty owner (owns nothing; `valid()` is false).
  UniqueFd() noexcept = default;
  /// @brief Adopt @p fd.
  /// @param fd  An open descriptor, or -1 for none.
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  /// @return The descriptor, still owned here; -1 when empty.
  int get() const noexcept { return fd_; }
  /// @return Whether this owns a descriptor.
  bool valid() const noexcept { return fd_ >= 0; }
  /// @brief Give the descriptor up without closing it, as an import that
  ///        took it needs.
  /// @return The descriptor, now the caller's; -1 when empty.
  int release() noexcept { return std::exchange(fd_, -1); }
  /// @brief Close the descriptor owned, if any, and adopt @p fd.
  /// @param fd  An open descriptor, or -1 for none.
  void reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

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
/// contain its own, so the first match is the plainest: asked for device-local
/// alone, it is one the host cannot map wherever @p type_bits allows such a
/// type outside those, keeping the resource out of a discrete GPU's BAR window.
/// Where the only unmapped device-local type is one of those -- a mobile
/// GPU's lazily allocated memory -- it is a host-visible one; exclude
/// `HOST_VISIBLE` to refuse that instead.
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
/// Its memory comes from @p allocator, dedicated to it, and is placed as
/// @ref MemoryUsage::DeviceOnly is -- never host memory, and on a discrete
/// GPU never the BAR window -- within its heap's budget, which it then counts
/// against as every allocation does.
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
///         @ref Status::Code::InvalidArgument for 0 bytes or a moved-from
///         @p allocator; or a backend @ref Status
///         (`VK_ERROR_OUT_OF_DEVICE_MEMORY` when the heap is full or past
///         its budget).
VKC_VULKAN_API Result<ExportedBuffer> create_exported_buffer(
    const Device& device, Allocator& allocator, VkDeviceSize bytes);

}  // namespace volumetric_kit::core
