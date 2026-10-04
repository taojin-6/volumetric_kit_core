// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_batch.hpp
/// @brief One call's uploads, fills, copies, dispatches and readbacks,
///        recorded into one command buffer and submitted with one fence wait.
///
/// The host reaches device memory through this and nothing else: kernel
/// memory is @ref MemoryUsage::DeviceOnly and unmapped on every platform, so
/// the host only records commands, and unified memory runs the same path a
/// discrete GPU does.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Allocator;
class GpuStageScope;
class GpuTimer;
class Device;
class Image;
struct ComputeKernel;

/// @brief Records one call's device work -- uploads, fills, copies, ownership
///        acquires, dispatches, readbacks -- into a single command buffer,
///        submitted once and waited on once by @ref submit.
///
/// The commands run in the order they were recorded, each seeing every write
/// before it: a barrier precedes any command that could see an earlier one's
/// writes -- always around a dispatch, and between two transfers when they
/// share a buffer one of them writes -- and the last makes everything visible
/// to the host and to a renderer drawing the result, as far as the queue
/// family allows. Fills, uploads and copies into one buffer at rising,
/// disjoint offsets touch no byte twice, so a run of them needs none, once no
/// command in the run writes a copy's source. Each dispatch sits in a
/// debug-utils region named for its kernel.
///
/// **Host bytes cross only at the edges, and each way has one path.** An
/// @ref upload of up to 64 KiB, 4-byte aligned -- a frame's parameters -- is
/// written inline in the command buffer (`vkCmdUpdateBuffer`); a larger one,
/// such as a depth frame, is copied in through a staging buffer in host memory
/// the batch allocates, which @ref reserve_upload hands to a caller packing
/// its own bytes. A @ref readback is for small results, a count or a failure
/// tally: every readback of a batch is copied into one small host buffer
/// allocated at @ref submit. Nothing is read or written through a mapping of
/// the destination, so a buffer's memory type never changes what a batch
/// does.
///
/// **A failed call poisons the batch.** Each recording call returns its own
/// refusal, and every later call, @ref submit included, returns the first of
/// them and records or runs nothing, so a batch missing a command is never
/// submitted.
///
/// **A batch records handles, not objects.** Every @ref Buffer and @ref Image
/// recorded, each dispatched kernel's pipeline, and every readback
/// destination must stay alive until @ref submit returns -- the objects may
/// move meanwhile -- and an image must stay in the layout it was copied in.
/// The one recorded thing a caller may change before then is a dispatch's
/// descriptor set, so @ref submit checks it and refuses the batch if it was
/// rewritten through any copy, or freed with its pool. A buffer its owner
/// replaces before then -- a grown upload or scratch buffer -- goes to
/// @ref retain. If the fence wait fails, the device may still run the batch:
/// it keeps the batch's staging with its command buffer until it is destroyed
/// (@ref Device::submit_single_time), and is best treated as lost. The batch
/// borrows its @ref Device and
/// @ref Allocator, which must outlive it. A batch belongs to one thread, but
/// batches on several threads may share a device and allocator, each
/// recording its own kernels and buffers.
///
/// An upload, a copy or a dispatch given a @ref GpuStageScope is timed: its
/// span covers the command alone, inside the dispatch's debug region, and
/// @ref submit resolves it once the fence has signalled, for the scope to
/// publish when it closes.
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
  /// @param stage   Optional span around the write, so a stage's device time
  ///                counts moving its input; it must outlive @ref submit.
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
  /// `VK_QUEUE_FAMILY_EXTERNAL`. For another Vulkan family, the writer
  /// records the releasing half on its own queue, the whole buffer from
  /// @p from to @ref Device::queue_family, and that work must have finished
  /// before @ref submit, as a waited fence ensures: the batch waits on no
  /// semaphore. Nothing is recorded when there is nothing to transfer: when
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
  /// @param stage       Optional span around the dispatch, on the scope's
  ///                    timer and label; null or inert is untimed. It must
  ///                    outlive @ref submit, which records and resolves it.
  /// @return OK; @ref Status::Code::InvalidArgument for @p groups past
  ///         @p max_groups, a null @p push with a size, a push size the
  ///         kernel does not take, or an unbuilt kernel; or a poisoned
  ///         batch's first refusal.
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
  ///         @ref dispatch refuses; or a poisoned batch's first refusal.
  Status dispatch_indirect(const ComputeKernel& kernel, const void* push,
                           std::uint32_t push_size, const Buffer& args,
                           VkDeviceSize offset, GpuStageScope* stage = nullptr);

  /// @brief Read @p bytes of @p src at @p offset, as they stand at this point
  ///        in the batch, into @p dst once @ref submit has waited.
  /// @param src     Needs `TRANSFER_SRC` usage.
  /// @param offset  The byte offset into @p src.
  /// @param bytes   How many; 0 records nothing.
  /// @param dst     Host memory of at least @p bytes, written by @ref submit;
  ///                it must stay valid until then.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past @p src,
  ///         a missing usage bit, a null @p dst, or a batch with no
  ///         allocator; or a poisoned batch's first refusal.
  Status readback(const Buffer& src, VkDeviceSize offset, VkDeviceSize bytes,
                  void* dst);

  /// @brief Keep @p buffer alive as the batch's own, freed with its staging
  ///        once @ref submit has waited.
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

  /// @brief Submit everything recorded as one command buffer, wait for it,
  ///        and fill every readback destination.
  ///
  /// Frees the batch's staging, and resolves the spans of every timer a
  /// command used; a resolve that fails is logged, not returned, as the batch
  /// succeeded. A submit that fails retires those timers
  /// (@ref GpuTimer::abandon): the device may still write their queries. An
  /// empty batch submits nothing. A batch is submitted at most once.
  /// @return OK; the first refusal a recording call returned;
  ///         @ref Status::Code::InvalidArgument for a second submit, a
  ///         moved-from batch, or a set rewritten through any copy, or freed,
  ///         after its dispatch was recorded; or a staging or Vulkan failure.
  Status submit();

  /// @return Whether @ref submit has run, whatever it returned.
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
    ImageCopy
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
    std::uint32_t to_family = 0;  // an acquire's destination family
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
    std::uint32_t width = 0;         // an ImageCopy's texels a row
    std::uint32_t height = 0;        // and rows
    GpuStageScope* stage = nullptr;  // its span, borrowed until submit
  };
  // A span record() opened, on the timer submit resolves.
  struct Span {
    GpuTimer* timer = nullptr;
    std::uint32_t id = 0;
  };

  Status check(Status status);
  Status usable() const;
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
  void record(VkCommandBuffer cmd, std::vector<Span>& spans) const;

  const Device* device_;
  Allocator* allocator_;
  std::vector<Op> ops_;
  std::vector<Buffer> staging_;      // and what retain() keeps
  VkBuffer zeros_ = VK_NULL_HANDLE;  // a staged word of zeros, made once
  Status status_;
  bool submitted_ = false;
};

}  // namespace volumetric_kit::core
