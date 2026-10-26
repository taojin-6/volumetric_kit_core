// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file host_ring.hpp
/// @brief Persistent slots for the bytes a stage moves between host and
///        device every frame, allocated once.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class CommandBatch;
class Device;

namespace detail {
struct RingSlot;  // a slot's buffers and its lease (src, internal)
}  // namespace detail

/// @brief Parameters for @ref HostRing::create.
///
/// @code
/// HostRingDesc desc;
/// desc.slot_bytes = 64 << 10;
/// desc.name = "codec.decode_input";
/// @endcode
struct HostRingDesc {
  /// Bytes each slot holds at create; non-zero. @ref HostRing::claim grows a
  /// free slot that is too small.
  VkDeviceSize slot_bytes = 0;
  /// How many slots; non-zero: one per batch in flight that binds the ring,
  /// plus one the host fills meanwhile.
  std::uint32_t slots = 2;
  /// The slots' debug name, `library.buffer`; borrowed (a string literal).
  /// Null leaves them unnamed.
  const char* name = nullptr;
  /// How the host touches the slots. It places their mapped buffer within the
  /// shape the device gives the ring (@ref HostRing::in_place), never changes
  /// the shape:
  /// - `HostAccess::Random`: the host reads as well as writes -- results, or
  ///   an input it parses in place. Cached memory.
  /// - `HostAccess::SequentialWrite`: the host fills each slot front to back
  ///   and never reads it -- a frame copied in for kernels. Write-combined
  ///   wherever the shape's mapped memory has an uncached type (system RAM
  ///   staging on a discrete GPU), which the host fills and the GPU copies
  ///   faster; else the same cached type as `Random`. Its slots are inputs
  ///   only: @ref CommandBatch::output refuses them.
  ///
  /// Unlike @ref BufferDesc::host_access, the default is `Random`: a ring
  /// that binds an output reads it.
  HostAccess host_access = HostAccess::Random;
};

/// @brief One slot of a @ref HostRing: host-mapped bytes that a batch binds
///        with @ref CommandBatch::input or @ref CommandBatch::output.
///
/// A copyable handle, which may be copied and dropped on any thread. The slot
/// is free for @ref HostRing::claim once every copy is dropped and every batch
/// that bound it resolved (its @ref CommandBatch::submit returned, a wait of
/// its @ref PendingBatch saw the work complete, or the batch was destroyed).
/// Whatever a thread did with the slot's bytes before dropping its copy
/// happens before the claim that next returns the slot. A batch whose wait
/// failed leaves the slot to the device until the device is destroyed. Drop a
/// slot once its frame is done.
///
/// The mapping is coherent, so nothing is flushed: kernels read the bytes as
/// they stand at the submit, and bytes they write are there once the batch
/// has resolved. How it is cached follows the ring's
/// @ref HostRingDesc::host_access (@ref memory_info says which type it took):
/// - `HostAccess::Random`: cached. The host writes what kernels read and
///   reads what they wrote.
/// - `HostAccess::SequentialWrite`: write-combined wherever the slot's
///   placement has an uncached type, else cached. The host writes the bytes
///   front to back and never reads them; a read through @ref data is not
///   refused, and its result is undefined, as
///   @ref HostAccess::SequentialWrite says. The slot is an input only.
///
/// A slot may outlive its ring; the ring's device must outlive both.
///
/// @code
/// VKC_ASSIGN(HostSlot slot, ring->claim(record_bytes));
/// // The I/O lands in the slot.
/// VKC_TRY(reader.read(record, slot.data(), slot.capacity()));
/// @endcode
class VKC_VULKAN_API HostSlot {
 public:
  /// @brief Construct an empty slot; @ref valid is false.
  HostSlot() noexcept = default;
  /// @return The host mapping, @ref capacity bytes; null when empty.
  std::uint8_t* data() const noexcept { return valid() ? data_ : nullptr; }
  /// @return Its bytes: at least what @ref HostRing::claim asked for; 0 when
  ///         empty.
  VkDeviceSize capacity() const noexcept { return valid() ? capacity_ : 0; }
  /// @return Whether this holds a slot.
  bool valid() const noexcept { return slot_ != nullptr; }
  /// @return The memory type @ref data maps -- the slot's own buffer in
  ///         place, its staging where copied -- as @ref Buffer::memory_info
  ///         reports it; empty when the slot is.
  std::optional<MemoryInfo> memory_info() const noexcept;

 private:
  friend class CommandBatch;
  friend class HostRing;
  std::shared_ptr<detail::RingSlot> slot_;
  // Cached at claim: a slot grows only while free.
  std::uint8_t* data_ = nullptr;
  VkDeviceSize capacity_ = 0;
};

/// @brief Persistent host<->device slots for a stage's per-frame bytes: its
///        input as the I/O delivers it, and the results the host decides on.
///        Nothing is allocated per batch.
///
/// The device's memory gives every slot one of two shapes, fixed at
/// @ref create (@ref in_place):
/// - **In place**, where device-mapped memory is cached, as unified memory's
///   one pool is: one `MemoryUsage::DeviceMapped` storage buffer, placed with
///   the ring's @ref HostRingDesc::host_access, which kernels bind and the
///   host maps.
/// - **Copied** everywhere else: a `MemoryUsage::Staging` buffer placed with
///   the ring's access (cached for `HostAccess::Random`; write-combined for
///   `HostAccess::SequentialWrite` where system RAM has an uncached type) and
///   a `MemoryUsage::DeviceOnly` mirror of the same size.
///   @ref CommandBatch::input copies a range in, and
///   @ref CommandBatch::output copies one out after the batch's last command.
///   The BAR window is never used, a write-only ring's included.
///
/// Kernel memory stays `DeviceOnly` and unmapped, and no shader reads host
/// memory. Slots are exclusive to the device's queue family. Each slot placed,
/// at create or when a claim grows one, counts in
/// @ref DeviceCounters::staging_buffers.
///
/// @ref claim runs on one thread at a time. A slot may be dropped on any
/// thread: the last copy's drop marks the slot free with a release store, and
/// @ref claim reads the mark with an acquire load.
///
/// @warning The @ref Device and @ref Allocator passed to @ref create must
///          outlive the ring.
///
/// @code
/// HostRingDesc desc;
/// desc.slot_bytes = 64 << 10;
/// desc.name = "codec.decode_input";
/// VKC_ASSIGN(std::unique_ptr<HostRing> inputs,
///            HostRing::create(device, allocator, desc));
/// VKC_ASSIGN(HostSlot slot, inputs->claim(record_bytes));
/// std::memcpy(slot.data(), record, record_bytes);  // or read straight into it
/// CommandBatch batch(device, allocator);
/// VKC_ASSIGN(const VkBuffer bytes, batch.input(slot, 0, record_bytes));
/// kernel.set.write_storage_buffer(0, bytes, 0, record_bytes);
/// VKC_TRY(batch.dispatch(kernel, &push, sizeof(push), groups, max_groups));
/// VKC_TRY(batch.submit());
/// @endcode
class VKC_VULKAN_API HostRing {
 public:
  /// @brief Allocate @p desc's slots, in place or copied as @p device's memory
  ///        decides (@ref in_place).
  /// @param device     The device whose kernels bind the slots.
  /// @param allocator  Allocates the slots, now and when @ref claim grows one.
  /// @param desc       Size, count, name and host access.
  /// @return The ring; @ref Status::Code::InvalidArgument for a zero
  ///         `slot_bytes` or `slots`, or a moved-from device or allocator; or
  ///         an allocation's failure. Nothing is placed elsewhere instead.
  static Result<std::unique_ptr<HostRing>> create(const Device& device,
                                                  Allocator& allocator,
                                                  const HostRingDesc& desc);

  ~HostRing();
  HostRing(const HostRing&) = delete;
  HostRing& operator=(const HostRing&) = delete;
  HostRing(HostRing&&) = delete;
  HostRing& operator=(HostRing&&) = delete;

  /// @return Whether kernels bind the slots themselves, with no copy: where
  ///         the device's `DeviceMapped` memory is cached
  ///         (@ref PhysicalDeviceInfo::device_mapped_cached), whatever the
  ///         ring's host access. A discrete GPU copies, with or without
  ///         Resizable BAR: the host reads VRAM uncached, and write-combined
  ///         staging plus one copy beat writing VRAM through the BAR
  ///         (DECISIONS.md, "Where memory lives").
  bool in_place() const noexcept { return in_place_; }

  /// @brief A free slot of at least @p bytes. Never waits.
  ///
  /// The smallest free slot that holds @p bytes is returned as it is, with its
  /// last use's contents. When none does, the smallest free slot is replaced
  /// by one of 1.5x @p bytes: its old buffers are freed first, and its
  /// contents are not kept. A held slot is never touched.
  /// @param bytes  What the caller will write or read; non-zero.
  /// @return The slot; @ref Status::Code::InvalidArgument for 0 bytes, or
  ///         when every slot is held (naming the count); or a grow's
  ///         allocation failure, which leaves that slot empty for a later
  ///         claim to grow. Allocating the lease can also fail without a
  ///         grow; the slot stays free for a later claim.
  Result<HostSlot> claim(VkDeviceSize bytes);

 private:
  HostRing(const Device& device, Allocator& allocator,
           const HostRingDesc& desc);
  // Allocates `slot`'s buffers for `bytes` with the ring's access, names them
  // and counts the slot.
  Status place(detail::RingSlot& slot, VkDeviceSize bytes);

  const Device& device_;
  Allocator& allocator_;
  const char* name_;
  HostAccess access_;
  bool in_place_;
  // free: !RingSlot::held (acquire load)
  std::vector<std::shared_ptr<detail::RingSlot>> slots_;
};

}  // namespace volumetric_kit::core
