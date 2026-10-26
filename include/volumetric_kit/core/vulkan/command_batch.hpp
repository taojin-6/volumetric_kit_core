// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_batch.hpp
/// @brief One call's uploads, fills, copies, dispatches and readbacks,
///        recorded into one command buffer and submitted with one fence wait
///        -- or submitted without it, ordered by timeline semaphores.
///
/// The host reaches device memory through this and nothing else: kernel
/// memory is @ref MemoryUsage::DeviceOnly and unmapped on every platform. Host
/// bytes cross at the batch's edges, per call (@ref CommandBatch::upload,
/// @ref CommandBatch::readback) or through a persistent @ref HostRing slot it
/// binds (@ref CommandBatch::input, @ref CommandBatch::output).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Allocator;
class HostSlot;
class Image;
struct ComputeKernel;

/// @brief A @ref CommandBatch submitted without waiting: what
///        @ref CommandBatch::submit_async returns.
///
/// Holds the batch's staging and readback memory, and the @ref HostRing slots
/// it binds, until a @ref wait sees its work complete; that wait fills every
/// readback destination, once, and frees them. Until then everything the batch
/// recorded -- buffers, images in their layouts, the dispatched kernels'
/// pipelines -- and every readback destination must stay alive. So must each
/// dispatched kernel's descriptor set, unwritten and unfreed, until a wait sees
/// the work complete: that wait refuses the batch if one was rewritten through
/// any copy, or freed, since the submit. Destroying an unfinished one waits for
/// its work, with no limit, as @ref Device::PendingSubmit says -- so set every
/// value it waits for on every path -- and leaves its readback destinations
/// unwritten. Used from one thread at a time, which need not be the one that
/// submitted it: no call -- @ref wait, @ref ready, @ref in_flight, a move --
/// may overlap another on the same batch.
///
/// A timed batch holds its tiles' query range too, and its wait reads them:
/// the rows by name (@ref device_rows) and the span (@ref device_span), which
/// the caller folds into its metrics. Nothing points at a caller's
/// @ref StageMetrics while the batch is pending, so the caller may move it.
///
/// @warning The batch's @ref Device and @ref Allocator must outlive it.
///
/// @code
/// VKC_ASSIGN(PendingBatch fused,
///            batch.submit_async({{&prepared, n}}, {{&fused_timeline, n}}));
/// // ... record and submit the next stage, which waits for fused_timeline ...
/// VKC_TRY(fused.wait());  // the batch's readbacks are written now
/// metrics.merge_device(fused.device_rows());
/// @endcode
class VKC_VULKAN_API PendingBatch {
 public:
  /// @brief Construct an empty pending batch: @ref ready, nothing to wait for.
  PendingBatch() noexcept = default;
  /// Waits for the work if it is unfinished, as @ref Device::PendingSubmit
  /// does; its query range goes back only if that wait saw the work
  /// complete.
  ~PendingBatch();
  PendingBatch(const PendingBatch&) = delete;
  PendingBatch& operator=(const PendingBatch&) = delete;
  PendingBatch(PendingBatch&& other) noexcept;
  /// Waits for this one's own unfinished work, its readbacks unwritten,
  /// before taking @p other's, as the destructor does.
  PendingBatch& operator=(PendingBatch&& other) noexcept;

  /// @return Whether @ref wait would return without blocking: the work has
  ///         completed or failed, or there is none. Never blocks, and writes
  ///         no readback.
  bool ready() const { return submit_.ready(); }

  /// @brief Wait until the work completes, or the timeout passes, then write
  ///        every readback destination, free the batch's staging, and read
  ///        its tiles.
  ///
  /// A wait that blocks reports the time to the scopes open on this thread
  /// (@ref note_blocked); one that finds the work complete does not.
  /// @param timeout_ns  The longest wait, in nanoseconds.
  /// @return As @ref Device::PendingSubmit::wait: OK once complete -- the
  ///         destinations written by the first such call only -- a
  ///         `VK_TIMEOUT` status (@ref is_timeout) with the work still
  ///         pending; the failure, which leaves the destinations unwritten;
  ///         or, once complete, @ref Status::Code::InvalidArgument when a
  ///         dispatched kernel's descriptor set was rewritten or freed since
  ///         the submit -- the work may have run on the later binding, so the
  ///         destinations are left unwritten. Every later call returns a
  ///         failure again.
  Status wait(std::uint64_t timeout_ns = UINT64_MAX);

  /// @return Whether the device may still run the work, as
  ///         @ref Device::PendingSubmit::in_flight says: what the batch
  ///         recorded must stay alive while it does.
  bool in_flight() const noexcept { return submit_.in_flight(); }

  /// @return The batch's device rows by name, once a wait has seen the work
  ///         complete: one for every scope its tiles count to, whichever
  ///         set opened it -- so fold them into one set, once --
  ///         @ref DeviceTime::Measured, @ref DeviceTime::NotTimed for what
  ///         ran untimed, or @ref DeviceTime::Unavailable on a timer that
  ///         cannot time; every row @ref DeviceTime::NotTimed after a failed
  ///         wait. Empty before, and for a batch that timed nothing
  ///         (@ref CommandBatch::time).
  const std::vector<StageRow>& device_rows() const noexcept {
    return tiles_.rows();
  }

  /// @return The batch's device span, first tile to last, once a wait has
  ///         seen the work complete and its timestamps were readable; on the
  ///         host's clock too where the device is calibrated. Empty
  ///         otherwise.
  std::optional<DeviceSpan> device_span() const noexcept {
    return tiles_.span();
  }

 private:
  friend class CommandBatch;

  // Where a readback lands: its slice of the readback memory, and the
  // caller's destination.
  struct Readback {
    void* dst = nullptr;
    VkDeviceSize offset = 0;
    VkDeviceSize bytes = 0;
  };
  // A dispatched kernel's set, and its write count at the submit.
  struct UsedSet {
    DescriptorSet set;
    std::uint64_t writes = 0;
  };

  // Copies each readback's slice of `memory` to its destination: the one
  // write-out both submits make.
  static void write_readbacks(const void* memory,
                              const std::vector<Readback>& readbacks);

  // The destructor's and a move assignment's: waits for unfinished work,
  // then gives up the unread tiles, keeping the range from reuse if the
  // device may still write it.
  void retire() noexcept;

  GpuTiles tiles_;
  Device::PendingSubmit submit_;
  // The batch's staging and readback memory, which the submit's keep_alive
  // shares; readback_memory_ is the mapping inside it.
  std::shared_ptr<void> retained_;
  const void* readback_memory_ = nullptr;
  std::vector<Readback> readbacks_;
  std::vector<UsedSet> sets_;
  Status failed_;  // a failed wait's, returned again
};

/// @brief Records one call's device work -- uploads, fills, copies, ownership
///        transfers, dispatches, readbacks -- into a single command buffer,
///        submitted once and waited on once by @ref submit, or submitted by
///        @ref submit_async and waited on through its @ref PendingBatch.
///
/// The commands run in the order they were recorded, each seeing every write
/// before it: a barrier precedes any command that could see an earlier one's
/// writes -- always around a dispatch, and between two transfers when they
/// share a buffer one of them writes -- and the last makes everything visible
/// to the host and to later commands on the same queue. On a graphics queue
/// this includes shader uniforms and storage buffers, as well as vertex,
/// index and indirect reads. A reader on another queue needs its own memory
/// dependency, such as a semaphore wait, even within the same queue family.
/// Fills, uploads and copies into one buffer at rising,
/// disjoint offsets touch no byte twice, so a run of them needs none, once no
/// command in the run writes a copy's source. Each dispatch sits in a
/// debug-utils region named for its kernel.
///
/// **Host bytes cross only at the edges.** Per call, each way has one path: an
/// @ref upload of up to 64 KiB, 4-byte aligned -- a frame's parameters -- is
/// written inline in the command buffer (`vkCmdUpdateBuffer`); a larger one,
/// such as a depth frame, is copied in through a staging buffer in host memory
/// the batch allocates, which @ref reserve_upload hands to a caller packing
/// its own bytes. A @ref readback is for small results, a count or a failure
/// tally: every readback of a batch is copied into one small host buffer
/// allocated at @ref submit. Nothing is read or written through a mapping of
/// the destination, so a buffer's memory type never changes what a batch
/// does. Per frame, with nothing allocated: a @ref HostRing slot, bound in
/// place where @ref HostRing::in_place says so and otherwise copied once
/// through its device-only mirror (@ref input, @ref output). Outside a test
/// process (@ref output), only a slot's own shape changes what a batch
/// records.
///
/// **A failed call poisons the batch.** Each recording call returns its own
/// refusal, and every later call, @ref submit included, returns the first of
/// them and records or runs nothing, so a batch missing a command is never
/// submitted.
///
/// **A batch records handles, not objects.** Every @ref Buffer and @ref Image
/// recorded, each dispatched kernel's pipeline, and every readback
/// destination must stay alive until the work completes -- until @ref submit
/// returns, or a wait of @ref submit_async's @ref PendingBatch sees it
/// complete; the objects may move meanwhile -- and an image must stay in the
/// layout it was copied in. The one recorded thing a caller may change
/// before the batch is submitted is a dispatch's descriptor set, so both
/// submits check it and refuse the batch if it was rewritten through any
/// copy, or freed with its pool; from the submit until the work completes it
/// must not change either, and @ref submit_async's @ref PendingBatch checks
/// that at its wait. A buffer its owner
/// replaces before then -- a grown upload or scratch buffer -- goes to
/// @ref retain. If the fence wait fails, the device may still run the batch:
/// it keeps the batch's staging with its command buffer until it is destroyed
/// (@ref Device::submit_single_time), and is best treated as lost. The batch
/// borrows its @ref Device and
/// @ref Allocator, which must outlive it. A batch belongs to one thread, but
/// batches on several threads may share a device and allocator, each
/// recording its own kernels and buffers.
///
/// **Device time is tiled.** An upload, a copy or a dispatch given a
/// @ref GpuStageScope is tagged with it, and a timed batch -- one with a
/// tagged command, or @ref time -- writes a timestamp before its first
/// command, at each change of tag, and after its last (@ref GpuTiles). A
/// command without a tag joins the open tile, so a stage's device time
/// includes its fills, barriers and gaps, and the tiles add up to the batch.
/// A tile counts to its tag's scope and to every scope open around it when
/// the command was recorded. @ref submit publishes into those still open on
/// its thread, at their close; @ref submit_async's @ref PendingBatch returns
/// the rows by name after its wait. A batch with no tag and no @ref time
/// writes no timestamp.
///
/// @code
/// CommandBatch batch(device, allocator);
/// VKC_TRY(batch.upload(params, 0, &frame_params, sizeof(frame_params)));
/// VKC_TRY(batch.dispatch(integrate, &push, sizeof(push), groups, max_groups));
/// VKC_TRY(batch.readback(counter, 0, sizeof(count), &count));
/// VKC_TRY(batch.submit());
/// @endcode
class VKC_VULKAN_API CommandBatch {
 public:
  /// The largest upload written inline in the command buffer
  /// (`vkCmdUpdateBuffer`'s limit); a larger one is staged.
  static constexpr VkDeviceSize kMaxInlineUpload = 65536;

  /// @brief Start a batch on @p device, staging through @p allocator. A
  ///        moved-from device or allocator poisons it.
  /// @param device     The device; it must outlive the batch.
  /// @param allocator  Allocates staging and readback memory; it must outlive
  ///                   the batch.
  CommandBatch(const Device& device, Allocator& allocator);
  /// @brief Start a batch that allocates nothing: a staged upload and a
  ///        readback are refused. What @ref dispatch "dispatch()" runs on.
  /// @param device  The device; it must outlive the batch.
  explicit CommandBatch(const Device& device);
  ~CommandBatch();
  CommandBatch(const CommandBatch&) = delete;
  CommandBatch& operator=(const CommandBatch&) = delete;
  CommandBatch(CommandBatch&& other) noexcept;
  CommandBatch& operator=(CommandBatch&& other) noexcept;

  /// @brief Write @p bytes from @p src into @p dst at @p offset.
  ///
  /// Inline when @p bytes is at most @ref kMaxInlineUpload and it and
  /// @p offset are multiples of 4; staged otherwise. @p src is copied before
  /// this returns.
  /// @param dst     Needs `TRANSFER_DST` usage.
  /// @param offset  The byte offset into @p dst.
  /// @param src     The bytes; null only when @p bytes is 0.
  /// @param bytes   How many; 0 records nothing.
  /// @param stage   Optional tag: the write counts to the scope's tile, so a
  ///                stage's device time counts moving its input; null or
  ///                inert leaves it untagged, in the open tile. Its timer
  ///                must outlive @ref submit; the scope need not.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past @p dst,
  ///         a missing usage bit, a null @p src, or a staged upload on a
  ///         batch with no allocator; a staging allocation's failure; or a
  ///         poisoned batch's first refusal.
  Status upload(const Buffer& dst, VkDeviceSize offset, const void* src,
                VkDeviceSize bytes, GpuStageScope* stage = nullptr);

  /// @brief Stage @p bytes for @p dst at @p offset and return the staging,
  ///        for the caller to fill before @ref submit.
  ///
  /// An upload the caller packs itself -- strided rows, several planes --
  /// written once and copied up as one command. Bytes left unwritten go up
  /// undefined.
  /// @param dst     Needs `TRANSFER_DST` usage.
  /// @param offset  The byte offset into @p dst.
  /// @param bytes   How many; not 0.
  /// @param stage   As @ref upload.
  /// @return The @p bytes to write, valid until @ref submit;
  ///         @ref Status::Code::InvalidArgument for 0 bytes, a range past
  ///         @p dst, a missing usage bit or a batch with no allocator; a
  ///         staging allocation's failure; or a poisoned batch's first
  ///         refusal.
  Result<void*> reserve_upload(const Buffer& dst, VkDeviceSize offset,
                               VkDeviceSize bytes,
                               GpuStageScope* stage = nullptr);

  /// @brief Set @p bytes of @p dst at @p offset to the repeated word
  ///        @p value (`vkCmdFillBuffer`).
  /// @param dst     Needs `TRANSFER_DST` usage.
  /// @param offset  The byte offset; a multiple of 4.
  /// @param bytes   A multiple of 4; 0 records nothing.
  /// @param value   The 32-bit word written.
  /// @return OK; @ref Status::Code::InvalidArgument for a misaligned or
  ///         out-of-range fill or a missing usage bit; or a poisoned batch's
  ///         first refusal.
  Status fill(const Buffer& dst, VkDeviceSize offset, VkDeviceSize bytes,
              std::uint32_t value);

  /// @brief Set @p bytes of @p dst at @p offset to zero, at any alignment: a
  ///        @ref fill for the whole words, and a copy for an unaligned edge
  ///        from one small buffer of zeros the batch stages once, however
  ///        many edges it zeroes.
  /// @param dst     Needs `TRANSFER_DST` usage.
  /// @param offset  The byte offset into @p dst.
  /// @param bytes   How many; 0 records nothing.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past @p dst,
  ///         a missing usage bit, or an unaligned edge on a batch with no
  ///         allocator; the zeros' allocation's failure; or a poisoned
  ///         batch's first refusal.
  Status zero(const Buffer& dst, VkDeviceSize offset, VkDeviceSize bytes);

  /// @brief Copy @p bytes from @p src to @p dst on the device.
  /// @param src         Needs `TRANSFER_SRC` usage.
  /// @param src_offset  The byte offset into @p src.
  /// @param dst         Needs `TRANSFER_DST` usage.
  /// @param dst_offset  The byte offset into @p dst.
  /// @param bytes       How many; 0 records nothing.
  /// @param stage       As @ref upload.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past either
  ///         buffer, overlapping ranges of one buffer or a missing usage bit;
  ///         or a poisoned batch's first refusal.
  Status copy(const Buffer& src, VkDeviceSize src_offset, const Buffer& dst,
              VkDeviceSize dst_offset, VkDeviceSize bytes,
              GpuStageScope* stage = nullptr);

  /// @brief Copy @p width x @p height texels of @p src, from its corner, into
  ///        @p dst at @p dst_offset, rows packed (`vkCmdCopyImageToBuffer`).
  ///
  /// Reads mip 0, layer 0 (the first slice of a 3D image), in the layout
  /// @ref Image::layout records now, which the image's writer left it in.
  /// The handle and layout are taken here, so the image must stay alive, and
  /// in that layout, until @ref submit returns; the batch cannot see a later
  /// transition.
  /// @param src         A single-sample image of an uncompressed 8-, 16- or
  ///                    32-bit-channel color format (`R8_UNORM` through
  ///                    `R32G32B32A32_SFLOAT`), with `TRANSFER_SRC` usage,
  ///                    in `GENERAL` or `TRANSFER_SRC_OPTIMAL`, its writer
  ///                    finished.
  /// @param width       Texels a row; not 0, and at most the image's width.
  /// @param height      Rows; not 0, and at most the image's height.
  /// @param dst         Needs `TRANSFER_DST` usage.
  /// @param dst_offset  The byte offset into @p dst; a multiple of 4 and of
  ///                    the texel size.
  /// @param stage       As @ref upload.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty image or one
  ///         of another format, sample count or layout, a region empty or
  ///         past it, a misaligned offset or a range past @p dst, or a missing
  ///         usage bit; or a poisoned batch's first refusal.
  Status copy(const Image& src, std::uint32_t width, std::uint32_t height,
              const Buffer& dst, VkDeviceSize dst_offset,
              GpuStageScope* stage = nullptr);

  /// @brief Take @p buffer over from the queue family @p from before the
  ///        commands recorded after this use it: the acquiring half of a
  ///        queue-family ownership transfer.
  ///
  /// Reading an `EXCLUSIVE` buffer that another family wrote is undefined
  /// without one, and so is reading memory an API outside Vulkan wrote --
  /// CUDA through an imported allocation -- which comes from
  /// `VK_QUEUE_FAMILY_EXTERNAL`. The writer records the releasing half --
  /// another Vulkan family's batch by @ref release, the whole buffer from
  /// @p from to @ref Device::queue_family -- and its work must have finished
  /// before the submit, as a waited fence or a synchronized CUDA stream
  /// ensures, or, for another Vulkan family's, set a timeline value
  /// @ref submit_async waits for. Nothing is recorded when there
  /// is nothing to transfer: when
  /// @p from is `VK_QUEUE_FAMILY_IGNORED` or this device's family, or when a
  /// `CONCURRENT` buffer was written on another Vulkan family (it is shared
  /// already).
  ///
  /// @code
  /// VKC_TRY(batch.acquire(imported, VK_QUEUE_FAMILY_EXTERNAL));  // CUDA wrote
  /// VKC_TRY(batch.dispatch(kernel, &push, sizeof(push), groups, max_groups));
  /// @endcode
  /// @param buffer  The buffer; it must stay alive until @ref submit returns.
  /// @param from    The family that last wrote it: one of the device's,
  ///                `VK_QUEUE_FAMILY_EXTERNAL` or `VK_QUEUE_FAMILY_IGNORED`.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty @p buffer or
  ///         a @p from that is none of those; or a poisoned batch's first
  ///         refusal.
  Status acquire(const Buffer& buffer, std::uint32_t from);

  /// @brief Hand @p buffer over to the queue family @p to once the batch's
  ///        commands are done: the releasing half of a queue-family ownership
  ///        transfer.
  ///
  /// Recorded after every command of the batch, whenever it is called, so a
  /// command recorded after it still runs before the buffer leaves. Its
  /// writes are made available to @p to, which takes the buffer over once
  /// the work has completed, or after a timeline value @ref submit_async
  /// sets: another family by its own @ref acquire, an API
  /// outside Vulkan from `VK_QUEUE_FAMILY_EXTERNAL` -- CUDA writing the next
  /// picture into an exported buffer, which a later batch acquires back.
  /// Nothing is recorded when there is nothing to transfer: when @p to is
  /// `VK_QUEUE_FAMILY_IGNORED` or this device's family, or when a
  /// `CONCURRENT` buffer goes to another Vulkan family (it is shared
  /// already).
  ///
  /// @code
  /// VKC_TRY(batch.acquire(frame, VK_QUEUE_FAMILY_EXTERNAL));  // CUDA wrote
  /// VKC_TRY(batch.dispatch(convert, &push, sizeof(push), groups, max_groups));
  /// VKC_TRY(batch.release(frame, VK_QUEUE_FAMILY_EXTERNAL));  // CUDA's again
  /// @endcode
  /// @param buffer  The buffer; it must stay alive until @ref submit returns.
  /// @param to      The family that uses it next: one of the device's,
  ///                `VK_QUEUE_FAMILY_EXTERNAL` or `VK_QUEUE_FAMILY_IGNORED`.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty @p buffer or
  ///         a @p to that is none of those; or a poisoned batch's first
  ///         refusal.
  Status release(const Buffer& buffer, std::uint32_t to);

  /// @brief Record a 1-D dispatch of @p kernel over @p groups workgroups.
  ///
  /// The kernel's pipeline, name and set are taken here, so the kernel may
  /// move, or have another set assigned, before @ref submit, which binds what
  /// was taken; its pipeline must stay alive until then.
  /// @param kernel      A built kernel whose set is written. The set is bound
  ///                    when @ref submit records, so rewriting it through any
  ///                    copy, or freeing its pool, before then makes
  ///                    @ref submit refuse the batch.
  /// @param push        Push-constant bytes, copied here; null only when
  ///                    @p push_size is 0.
  /// @param push_size   A multiple of 4, at most the kernel's
  ///                    @ref ComputeKernel::push_bytes; 0 pushes nothing.
  /// @param groups      Workgroups along x.
  /// @param max_groups  The device's `maxComputeWorkGroupCount[0]`: an
  ///                    oversized grid is invalid on a minimum-spec driver,
  ///                    and clamping it would drop work silently.
  /// @param stage       Optional tag, as @ref upload's.
  /// @return OK; @ref Status::Code::InvalidArgument for @p groups past
  ///         @p max_groups, a null @p push with a size, a push size the
  ///         kernel does not take, or an unbuilt kernel;
  ///         @ref Status::Code::Unsupported when the queue cannot compute;
  ///         or a poisoned batch's first refusal.
  Status dispatch(const ComputeKernel& kernel, const void* push,
                  std::uint32_t push_size, std::uint32_t groups,
                  std::uint32_t max_groups, GpuStageScope* stage = nullptr);

  /// @brief @ref dispatch with @p set bound in place of the kernel's own.
  ///
  /// So one batch can dispatch a kernel several times over different
  /// buffers, each dispatch binding a set of its own of the kernel's layout
  /// (@ref KernelSets). The kernel's own set's rule holds for @p set:
  /// rewritten through any copy, or freed with its pool -- a
  /// @ref KernelSets::reserve that grows -- before @ref submit, the batch is
  /// refused.
  /// @param kernel      As @ref dispatch.
  /// @param set         A written set of @p kernel's layout. The batch keeps
  ///                    a copy, which shares the set's write count, so the
  ///                    caller's object may go. The layout is not checked;
  ///                    the validation layer names a mismatch.
  /// @param push        As @ref dispatch.
  /// @param push_size   As @ref dispatch.
  /// @param groups      As @ref dispatch.
  /// @param max_groups  As @ref dispatch.
  /// @param stage       As @ref dispatch.
  /// @return As @ref dispatch; @ref Status::Code::InvalidArgument also for an
  ///         empty @p set.
  Status dispatch(const ComputeKernel& kernel, const DescriptorSet& set,
                  const void* push, std::uint32_t push_size,
                  std::uint32_t groups, std::uint32_t max_groups,
                  GpuStageScope* stage = nullptr);

  /// @brief Record a dispatch of @p kernel whose workgroup counts the device
  ///        reads from @p args at @p offset (`vkCmdDispatchIndirect`), so a
  ///        count a kernel produced sizes the next without reaching the host.
  /// @param kernel     As @ref dispatch.
  /// @param push       As @ref dispatch.
  /// @param push_size  As @ref dispatch.
  /// @param args       Holds a `VkDispatchIndirectCommand`; needs
  ///                   `INDIRECT_BUFFER` usage. Counts past the device's
  ///                   limits are the writer's to prevent.
  /// @param offset     A multiple of 4.
  /// @param stage      As @ref dispatch.
  /// @return OK; @ref Status::Code::InvalidArgument for a misaligned or
  ///         out-of-range command, a missing usage bit, or a push or kernel
  ///         @ref dispatch refuses; @ref Status::Code::Unsupported when the
  ///         queue cannot compute; or a poisoned batch's first refusal.
  Status dispatch_indirect(const ComputeKernel& kernel, const void* push,
                           std::uint32_t push_size, const Buffer& args,
                           VkDeviceSize offset, GpuStageScope* stage = nullptr);

  /// @brief Read @p bytes of @p src at @p offset, as they stand at this point
  ///        in the batch, into @p dst once the work has completed.
  /// @param src     Needs `TRANSFER_SRC` usage.
  /// @param offset  The byte offset into @p src.
  /// @param bytes   How many; 0 records nothing.
  /// @param dst     Host memory of at least @p bytes, written by @ref submit,
  ///                or by the wait of @ref submit_async's @ref PendingBatch
  ///                that sees the work complete; it must stay valid until
  ///                then.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past @p src,
  ///         a missing usage bit, a null @p dst, or a batch with no
  ///         allocator; or a poisoned batch's first refusal.
  Status readback(const Buffer& src, VkDeviceSize offset, VkDeviceSize bytes,
                  void* dst);

  /// @brief Make @p bytes of @p slot, from @p offset, readable by the kernels
  ///        recorded after this, and return the buffer they bind.
  ///
  /// In place, that buffer is the slot itself and nothing is recorded.
  /// Copied, it is the slot's device-only mirror, which holds the slot's bytes
  /// at the same offsets, and this records one copy of the range into it,
  /// untagged, so it joins the open tile: the first, when called before the
  /// batch's first tagged command. Kernels read the bytes as the host left
  /// them at the submit, so write them first and not again until the batch
  /// resolves. Bind only inside the range, at offsets
  /// `minStorageBufferOffsetAlignment` allows. The batch holds @p slot until
  /// it resolves.
  ///
  /// @code
  /// VKC_ASSIGN(const VkBuffer frame, batch.input(slot, 0, used));
  /// decode.set.write_storage_buffer(1, frame, payload_at, payload_bytes);
  /// @endcode
  /// @param slot    A slot from a @ref HostRing on this batch's device.
  /// @param offset  The range's first byte in the slot.
  /// @param bytes   Its length; non-zero.
  /// @return The buffer to bind; @ref Status::Code::InvalidArgument for an
  ///         empty @p slot, zero @p bytes, a range past the slot or a
  ///         moved-from batch; @ref Status::Code::OutOfMemory if retaining
  ///         the slot or recording commands fails; or a poisoned batch's
  ///         first refusal. A recording failure poisons the batch.
  Result<VkBuffer> input(const HostSlot& slot, VkDeviceSize offset,
                         VkDeviceSize bytes);

  /// @brief Return the buffer kernels write @p bytes of results into, at
  ///        @p offset, for the host to read through @p slot once the batch
  ///        resolves.
  ///
  /// In place, that buffer is the slot itself, nothing is recorded, and the
  /// batch's final barrier makes the writes visible to the host. Copied, it
  /// is the slot's mirror, and this records a copy of the range into the slot
  /// after every other command of the batch, wherever it is called, as
  /// @ref release is recorded last. The range holds the results once
  /// @ref submit returns OK or a wait of @ref submit_async's
  /// @ref PendingBatch returns OK; after a failure it is undefined.
  ///
  /// Kernels must write every byte the host reads. While a test holds the
  /// core's test policy's `RingOutputPoison` (`core_test_policy`; the
  /// GoogleTest fixtures hold one), the range is poisoned before the batch
  /// runs: in place the host writes a poison word; copied, a fill of the
  /// mirror at the batch's front writes it, which costs one more barrier in a
  /// batch that would otherwise open with a dispatch. A byte the kernels skip
  /// then shows as the poison in that device's results.
  /// Elsewhere nothing is poisoned, and such a byte holds an earlier use's
  /// value. So an output's bytes are not also an @ref input. The batch holds
  /// @p slot until it resolves.
  ///
  /// @code
  /// VKC_ASSIGN(const VkBuffer verdict, batch.output(results, 0, 64));
  /// gather.set.write_storage_buffer(0, verdict, 0, 64);
  /// VKC_TRY(batch.dispatch(gather, nullptr, 0, 1, max_groups));
  /// @endcode
  /// @param slot    A slot from a @ref HostRing on this batch's device whose
  ///                @ref HostRingDesc::host_access is `HostAccess::Random`.
  /// @param offset  The range's first byte in the slot.
  /// @param bytes   Its length; non-zero.
  /// @return As @ref input; also @ref Status::Code::InvalidArgument for a
  ///         slot of a `HostAccess::SequentialWrite` ring, on every device,
  ///         refused before the batch holds the slot or poisons its range.
  Result<VkBuffer> output(const HostSlot& slot, VkDeviceSize offset,
                          VkDeviceSize bytes);

  /// @brief Keep @p buffer alive as the batch's own, freed with its staging
  ///        once the work has completed.
  ///
  /// For a buffer the recorded commands use that its owner replaces before
  /// @ref submit -- an upload or scratch buffer grown mid-call, as
  /// @ref StorageInput::buffer and @ref ensure_device_scratch do -- which
  /// would otherwise be freed under them. Taken on any batch, a poisoned or
  /// submitted one included, and freed with it if it never runs.
  ///
  /// @code
  /// batch.retain(std::move(upload));  // the commands so far still read it
  /// VKC_ASSIGN(upload, device_storage_buffer(allocator, bigger));
  /// @endcode
  /// @param buffer  The buffer; may be empty, which keeps nothing.
  void retain(Buffer buffer);

  /// @brief Time the batch's tiles in @p timer, even with no tagged command:
  ///        such a batch still has a span (@ref PendingBatch::device_span).
  ///
  /// Without it, a batch times in its first tagged command's timer. The
  /// timer's @ref GpuTimer::max_depth applies.
  /// @param timer  The timer; it must outlive the submit.
  void time(GpuTimer& timer) noexcept { timer_ = &timer; }

  /// @brief Submit everything recorded as one command buffer, wait for it,
  ///        and fill every readback destination.
  ///
  /// Frees the batch's staging, and reads its tiles once the fence has
  /// signalled, crediting them to the scopes still open on this thread that
  /// they count to. They read @ref DeviceTime::NotTimed when the batch
  /// failed, before the device or on it, or found no free query range; a
  /// range the device may still write is never reused. The fence wait reports
  /// its time to the scopes open on this thread (@ref note_blocked). An empty
  /// batch submits nothing. A batch is submitted at most once, by this or
  /// @ref submit_async.
  /// @return OK; the first refusal a recording call returned;
  ///         @ref Status::Code::InvalidArgument for a second submit, a
  ///         moved-from batch, or a set rewritten through any copy, or freed,
  ///         after its dispatch was recorded; or a staging or Vulkan failure.
  Status submit();

  /// @brief Submit everything recorded as one command buffer, after every
  ///        value in @p wait, setting every value in @p signal once it
  ///        completes, and return without waiting for it.
  ///
  /// The work orders against other submissions -- the stages of a pipeline,
  /// another library's on a shared device -- only through the values, as
  /// @ref Device::submit_pending says: nothing recorded starts before every
  /// wait is met and sees the writes of the submissions that met them, and a
  /// value set means every command has completed, its writes available. The
  /// host reads the readbacks, and frees the staging, through the returned
  /// @ref PendingBatch, whose wait does what @ref submit does after its own.
  /// An empty batch with no value to wait for or set submits nothing; one
  /// with values still submits, so a stage with no work still passes its
  /// value on.
  ///
  /// Two rules from @ref Device::submit_pending matter most here. Submit the
  /// batch that sets a value before any that waits for it: the batch ends in
  /// a barrier every later command on the queue waits for, so a batch
  /// waiting for a value only a later one sets hangs the queue. And until a
  /// value the host sets is set, work held for it holds every later wait on
  /// the queue -- @ref submit, @ref Device::wait_idle -- with it.
  ///
  /// Timed as @ref submit is: the batch holds its query range until the
  /// pending batch's wait reads it, which returns the rows by name
  /// (@ref PendingBatch::device_rows) rather than writing into a scope.
  ///
  /// @code
  /// VKC_ASSIGN(PendingBatch prepared, prep.submit_async({}, {{&frames, n}}));
  /// VKC_ASSIGN(PendingBatch fused, fuse.submit_async({{&frames, n}}, {}));
  /// VKC_TRY(fused.wait());
  /// @endcode
  /// @param wait    Values to reach before anything recorded starts.
  /// @param signal  Values to set once everything recorded completes, as
  ///                @ref Device::submit_pending allows them.
  /// @return The pending batch; what @ref submit refuses, for the same
  ///         reasons; what @ref Device::submit_pending refuses for the
  ///         values; or a staging or Vulkan failure. A refusal of the values
  ///         comes first and leaves the batch unsubmitted, for a submit with
  ///         corrected ones; any other refusal or failure leaves it
  ///         submitted. None runs any of it.
  Result<PendingBatch> submit_async(const std::vector<TimelinePoint>& wait,
                                    const std::vector<TimelinePoint>& signal);

  /// @return Whether @ref submit or @ref submit_async has run, whatever it
  ///         returned.
  bool submitted() const noexcept { return submitted_; }

 private:
  enum class Kind {
    Update,
    Copy,
    Fill,
    Dispatch,
    DispatchIndirect,
    Readback,
    Acquire,
    Release,
    ImageCopy,
    Output  // a copied ring output, recorded after every other command
  };
  // What a command needs at submit, taken when it is recorded: handles and
  // copies, never a pointer to a caller's object, which may move or go.
  struct Op {
    Kind kind = Kind::Copy;
    VkBuffer src = VK_NULL_HANDLE;
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize src_offset = 0;
    VkDeviceSize dst_offset = 0;
    VkDeviceSize bytes = 0;
    std::uint32_t value = 0;      // fill word, workgroup count, or from-family
    std::uint32_t to_family = 0;  // a transfer's destination family
    // A dispatch's kernel: its pipeline, the layout it binds and pushes
    // through, and its name, borrowed (a string literal) for the region.
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    const char* name = nullptr;
    // The set a dispatch binds: a copy, sharing the write count and seeing
    // the pool go, and its handle and write count when recorded.
    DescriptorSet set;
    VkDescriptorSet set_handle = VK_NULL_HANDLE;
    std::uint64_t set_writes = 0;
    std::vector<unsigned char> data;  // push constants, or an inline upload
    void* host_dst = nullptr;         // a readback's destination
    bool staged = false;              // a Copy from this batch's own staging
    VkImage image = VK_NULL_HANDLE;   // an ImageCopy's source
    VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t width = 0;   // an ImageCopy's texels a row
    std::uint32_t height = 0;  // and rows
    // Its tile: an index into keys_, taken when recorded, or untagged.
    std::uint32_t tile = GpuTiles::kNoKey;
  };

  Status check(Status status);
  Status usable() const;
  // The two submits' common start: refuses a moved-from batch, a second
  // submit and a poisoned batch, marking the batch submitted.
  Status begin_submit();
  // submit() up to the failure it credits to the scopes: the checks, the
  // readbacks' placement, the submit, its wait and the write-out.
  Status submit_blocking();
  // Refuses a dispatch whose set was rewritten or freed since it was
  // recorded, naming `call`.
  Status check_sets(const char* call) const;
  // Places every readback in one host buffer, at its own slice, listed in
  // `*slices`; null, and none, when there is none.
  Result<const Buffer*> place_readbacks(
      std::vector<PendingBatch::Readback>* slices);
  // An acquire from `family`, or a release to it.
  Status transfer(Kind kind, const Buffer& buffer, std::uint32_t family);
  static Status check_dispatch(const ComputeKernel& kernel, const void* push,
                               std::uint32_t push_size);
  static Op dispatch_op(Kind kind, const ComputeKernel& kernel,
                        const DescriptorSet& set, const void* push,
                        std::uint32_t push_size);
  // A host-visible buffer of `bytes`, held until the submit's wait is done.
  Result<const Buffer*> stage(VkDeviceSize bytes, bool upload);
  // Zeroes `bytes` (under 4) of `dst` at `offset` by a copy from zeros_.
  Status zero_edge(const Buffer& dst, VkDeviceSize offset, VkDeviceSize bytes);
  bool needs_barrier(std::size_t first, std::size_t i) const;
  // The tile key a command recorded now under `stage` counts to, made once
  // per scope; GpuTiles::kNoKey when untagged.
  std::uint32_t tag(GpuStageScope* stage);
  // The timer the batch times in, or null.
  GpuTimer* timing_timer() const noexcept {
    return timer_ != nullptr ? timer_ : tag_timer_;
  }
  // The tiles the commands make at `depth`: one, and one per change of tile.
  std::uint32_t tile_bound(std::uint32_t depth) const noexcept;
  // Records every command into `cmd`, marking `tiles`, when not null, at
  // each tagged command, and counts its dispatches and barriers on the
  // device; the recording both submits run.
  void record(VkCommandBuffer cmd, GpuTiles* tiles) const;
  // input()'s and output()'s checks, and the batch's hold on `slot`; an
  // `output` of a write-only ring's slot is refused.
  Status bind(const HostSlot& slot, VkDeviceSize offset, VkDeviceSize bytes,
              bool output, const char* call);

  const Device* device_;
  Allocator* allocator_;
  std::vector<Op> ops_;
  // The ring slots bound: moved into the submit's work, which holds them
  // until it resolves.
  std::vector<std::shared_ptr<const void>> slots_;
  std::vector<Buffer> staging_;      // and what retain() keeps
  VkBuffer zeros_ = VK_NULL_HANDLE;  // a staged word of zeros, made once
  // The tile keys the tagged commands count to, one per scope, and the
  // first tagged command's timer.
  std::vector<detail::TileKey> keys_;
  GpuTimer* tag_timer_ = nullptr;
  GpuTimer* timer_ = nullptr;  // time()'s
  Status status_;
  bool submitted_ = false;
};

}  // namespace volumetric_kit::core
