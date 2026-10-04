// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_batch.hpp
/// @brief One call's uploads, fills, copies, dispatches and readbacks,
///        recorded into one command buffer and submitted with one fence wait.
///
/// The host reaches device memory through this and nothing else: kernel
/// memory is device-local and unmapped on every platform, so the host only
/// records commands, and unified memory runs the same path a discrete GPU
/// does.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Allocator;
class DescriptorSet;
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
/// such as a depth frame, is copied in through a host-visible staging buffer
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
/// Every @ref Buffer recorded must stay alive until @ref submit returns, and
/// so must every readback destination. If the fence wait fails, the device
/// may still run the batch: the batch then keeps its staging for good, as
/// @ref Device::submit_single_time keeps its command buffer, and the device
/// is best treated as lost. The batch borrows its @ref Device and
/// @ref Allocator, which must outlive it. A batch belongs to one thread, but
/// batches on several threads may share a device and allocator, each
/// recording its own kernels and buffers.
///
/// TODO: V4 adds recon's optional `GpuStageScope*` span to @ref upload,
/// @ref reserve_upload, the copies and the dispatches, resolved at
/// @ref submit.
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
  /// @return OK; @ref Status::Code::InvalidArgument for a range past @p dst,
  ///         a missing usage bit, a null @p src, or a staged upload on a
  ///         batch with no allocator; a staging allocation's failure; or a
  ///         poisoned batch's first refusal.
  Status upload(const Buffer& dst, VkDeviceSize offset, const void* src,
                VkDeviceSize bytes);

  /// @brief Stage @p bytes for @p dst at @p offset and return the staging,
  ///        for the caller to fill before @ref submit.
  ///
  /// An upload the caller packs itself -- strided rows, several planes --
  /// written once and copied up as one command. Bytes left unwritten go up
  /// undefined.
  /// @param dst     Needs `TRANSFER_DST` usage.
  /// @param offset  The byte offset into @p dst.
  /// @param bytes   How many; not 0.
  /// @return The @p bytes to write, valid until @ref submit;
  ///         @ref Status::Code::InvalidArgument for 0 bytes, a range past
  ///         @p dst, a missing usage bit or a batch with no allocator; a
  ///         staging allocation's failure; or a poisoned batch's first
  ///         refusal.
  Result<void*> reserve_upload(const Buffer& dst, VkDeviceSize offset,
                               VkDeviceSize bytes);

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
  ///        @ref fill for the whole words, an @ref upload for an unaligned
  ///        edge.
  /// @param dst     Needs `TRANSFER_DST` usage.
  /// @param offset  The byte offset into @p dst.
  /// @param bytes   How many; 0 records nothing.
  /// @return As @ref fill and @ref upload.
  Status zero(const Buffer& dst, VkDeviceSize offset, VkDeviceSize bytes);

  /// @brief Copy @p bytes from @p src to @p dst on the device.
  /// @param src         Needs `TRANSFER_SRC` usage.
  /// @param src_offset  The byte offset into @p src.
  /// @param dst         Needs `TRANSFER_DST` usage.
  /// @param dst_offset  The byte offset into @p dst.
  /// @param bytes       How many; 0 records nothing.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past either
  ///         buffer, overlapping ranges of one buffer or a missing usage bit;
  ///         or a poisoned batch's first refusal.
  Status copy(const Buffer& src, VkDeviceSize src_offset, const Buffer& dst,
              VkDeviceSize dst_offset, VkDeviceSize bytes);

  /// @brief Copy @p width x @p height texels of @p src, from its corner, into
  ///        @p dst at @p dst_offset, rows packed (`vkCmdCopyImageToBuffer`).
  ///
  /// Reads mip 0, layer 0 (the first slice of a 3D image), in the layout
  /// @ref Image::layout records, which the image's writer left it in.
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
  /// @return OK; @ref Status::Code::InvalidArgument for an empty image or one
  ///         of another format, sample count or layout, a region empty or
  ///         past it, a misaligned offset or a range past @p dst, or a missing
  ///         usage bit; or a poisoned batch's first refusal.
  Status copy(const Image& src, std::uint32_t width, std::uint32_t height,
              const Buffer& dst, VkDeviceSize dst_offset);

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
  /// @param kernel      A built kernel whose set is written. The set is bound
  ///                    when @ref submit records, so rewriting it through any
  ///                    copy, or replacing it, before then makes @ref submit
  ///                    refuse the batch.
  /// @param push        Push-constant bytes, copied here; null only when
  ///                    @p push_size is 0.
  /// @param push_size   A multiple of 4, at most the kernel's
  ///                    @ref ComputeKernel::push_bytes; 0 pushes nothing.
  /// @param groups      Workgroups along x.
  /// @param max_groups  The device's `maxComputeWorkGroupCount[0]`: an
  ///                    oversized grid is invalid on a minimum-spec driver,
  ///                    and clamping it would drop work silently.
  /// @return OK; @ref Status::Code::InvalidArgument for @p groups past
  ///         @p max_groups, a null @p push with a size, a push size the
  ///         kernel does not take, or an unbuilt kernel; or a poisoned
  ///         batch's first refusal.
  Status dispatch(const ComputeKernel& kernel, const void* push,
                  std::uint32_t push_size, std::uint32_t groups,
                  std::uint32_t max_groups);

  /// @brief @ref dispatch with @p set bound in place of the kernel's own.
  ///
  /// So one batch can dispatch a kernel several times over different
  /// buffers, each dispatch binding a set of its own of the kernel's layout
  /// (@ref KernelSets). The kernel's own set's rule holds for @p set:
  /// rewritten through any copy, or replaced, before @ref submit, the batch
  /// is refused.
  /// @param kernel      As @ref dispatch.
  /// @param set         A written set of @p kernel's layout; it must stay
  ///                    alive until @ref submit returns, as the batch binds
  ///                    and checks that object, not a copy. The layout is not
  ///                    checked; the validation layer names a mismatch.
  /// @param push        As @ref dispatch.
  /// @param push_size   As @ref dispatch.
  /// @param groups      As @ref dispatch.
  /// @param max_groups  As @ref dispatch.
  /// @return As @ref dispatch; @ref Status::Code::InvalidArgument also for an
  ///         empty @p set.
  Status dispatch(const ComputeKernel& kernel, const DescriptorSet& set,
                  const void* push, std::uint32_t push_size,
                  std::uint32_t groups, std::uint32_t max_groups);
  /// A temporary set would be gone by @ref submit.
  Status dispatch(const ComputeKernel& kernel, DescriptorSet&& set,
                  const void* push, std::uint32_t push_size,
                  std::uint32_t groups, std::uint32_t max_groups) = delete;

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
  /// @return OK; @ref Status::Code::InvalidArgument for a misaligned or
  ///         out-of-range command, a missing usage bit, or a push or kernel
  ///         @ref dispatch refuses; or a poisoned batch's first refusal.
  Status dispatch_indirect(const ComputeKernel& kernel, const void* push,
                           std::uint32_t push_size, const Buffer& args,
                           VkDeviceSize offset);

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

  /// @brief Submit everything recorded as one command buffer, wait for it,
  ///        and fill every readback destination.
  ///
  /// Frees the batch's staging. An empty batch submits nothing. A batch is
  /// submitted at most once.
  /// @return OK; the first refusal a recording call returned;
  ///         @ref Status::Code::InvalidArgument for a second submit, a
  ///         moved-from batch, or a set rewritten through any copy or
  ///         replaced after its dispatch was recorded; or a staging or Vulkan
  ///         failure.
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
  struct Op {
    Kind kind = Kind::Copy;
    VkBuffer src = VK_NULL_HANDLE;
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize src_offset = 0;
    VkDeviceSize dst_offset = 0;
    VkDeviceSize bytes = 0;
    std::uint32_t value = 0;      // fill word, workgroup count, or from-family
    std::uint32_t to_family = 0;  // an acquire's destination family
    const ComputeKernel* kernel = nullptr;
    const DescriptorSet* set = nullptr;           // the set a dispatch binds
    VkDescriptorSet set_handle = VK_NULL_HANDLE;  // its handle when recorded
    std::uint64_t set_writes = 0;                 // its writes when recorded
    std::vector<unsigned char> data;  // push constants, or an inline upload
    void* host_dst = nullptr;         // a readback's destination
    bool staged = false;              // a Copy from this batch's own staging
    VkImage image = VK_NULL_HANDLE;   // an ImageCopy's source
    VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t width = 0;   // an ImageCopy's texels a row
    std::uint32_t height = 0;  // and rows
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
  bool needs_barrier(std::size_t first, std::size_t i) const;
  void record(VkCommandBuffer cmd) const;

  const Device* device_;
  Allocator* allocator_;
  std::vector<Op> ops_;
  std::vector<Buffer> staging_;
  Status status_;
  bool submitted_ = false;
};

}  // namespace volumetric_kit::core
