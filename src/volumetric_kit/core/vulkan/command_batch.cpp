// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/command_batch.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "command_batch_barriers.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/compute_kernel.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

// What a submit's work uses past the batch: the staging it copies through,
// and the query pools its spans write.
struct Retained {
  std::vector<Buffer> staging;
  std::vector<std::shared_ptr<void>> queries;
};

// `bytes` at `offset` lie inside `buffer`, written so neither sum can wrap.
Status in_range(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize bytes,
                const char* what) {
  if (!buffer.valid()) {
    return Status::invalid_argument(std::string("CommandBatch: ") + what +
                                    " buffer is empty");
  }
  if (offset > buffer.size() || bytes > buffer.size() - offset) {
    return Status::invalid_argument(std::string("CommandBatch: ") + what +
                                    " range lies past the buffer");
  }
  return {};
}

Status has_usage(const Buffer& buffer, VkBufferUsageFlags bit,
                 const char* what) {
  if ((buffer.usage() & bit) == 0) {
    return Status::invalid_argument(std::string("CommandBatch: ") + what);
  }
  return {};
}

// The bytes of one texel of a format copy(Image) accepts -- an uncompressed
// color format of 8-, 16- or 32-bit channels (command_batch.hpp) -- or 0 for
// any other. Narrower by design than the public texel_bytes (format.hpp),
// which sizes every flat-copyable format.
VkDeviceSize copyable_texel_bytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
      return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
      return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT:
      return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 16;
    default:
      return 0;
  }
}

}  // namespace

CommandBatch::CommandBatch(const Device& device)
    : device_(&device), allocator_(nullptr) {
  if (device.handle() == VK_NULL_HANDLE) {
    status_ =
        Status::invalid_argument("CommandBatch: the device is moved-from");
  }
}

CommandBatch::CommandBatch(const Device& device, Allocator& allocator)
    : CommandBatch(device) {
  allocator_ = &allocator;
  if (status_.ok() && !allocator.valid()) {
    status_ =
        Status::invalid_argument("CommandBatch: the allocator is moved-from");
  }
}

// Staging still held here never reached the device, or its submit failed
// before the device had it: submit hands its own to the device, which frees
// it after the wait, or after waiting again at its destruction.
CommandBatch::~CommandBatch() = default;

CommandBatch::CommandBatch(CommandBatch&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      allocator_(std::exchange(other.allocator_, nullptr)),
      ops_(std::exchange(other.ops_, {})),
      staging_(std::exchange(other.staging_, {})),
      zeros_(std::exchange(other.zeros_, VK_NULL_HANDLE)),
      status_(std::exchange(other.status_, Status{})),
      submitted_(std::exchange(other.submitted_, false)) {}

CommandBatch& CommandBatch::operator=(CommandBatch&& other) noexcept {
  if (this != &other) {
    device_ = std::exchange(other.device_, nullptr);
    allocator_ = std::exchange(other.allocator_, nullptr);
    ops_ = std::exchange(other.ops_, {});
    staging_ = std::exchange(other.staging_, {});
    zeros_ = std::exchange(other.zeros_, VK_NULL_HANDLE);
    status_ = std::exchange(other.status_, Status{});
    submitted_ = std::exchange(other.submitted_, false);
  }
  return *this;
}

Status CommandBatch::check(Status status) {
  if (!status.ok() && status_.ok()) status_ = status;
  return status;
}

Status CommandBatch::usable() const {
  if (device_ == nullptr) {
    return Status::invalid_argument("CommandBatch: the batch is moved-from");
  }
  if (!status_.ok()) return status_;
  if (submitted_) {
    return Status::invalid_argument("CommandBatch: already submitted");
  }
  return {};
}

Result<const Buffer*> CommandBatch::stage(VkDeviceSize bytes, bool upload) {
  if (allocator_ == nullptr) {
    return Status::invalid_argument(
        "CommandBatch: the batch has no allocator to stage through");
  }
  BufferDesc desc;
  desc.size = bytes;
  desc.usage = upload ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                      : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  desc.memory = MemoryUsage::Staging;
  // Write-combined for an upload; cached for a readback, which the host reads.
  desc.host_access = upload ? HostAccess::SequentialWrite : HostAccess::Random;
  VKC_ASSIGN(Buffer buffer, allocator_->create_buffer(desc));
  device_->set_object_name(VK_OBJECT_TYPE_BUFFER,
                           debug_object_handle(buffer.handle()),
                           upload ? "batch.upload" : "batch.readback");
  staging_.push_back(std::move(buffer));
  return &staging_.back();
}

Status CommandBatch::upload(const Buffer& dst, VkDeviceSize offset,
                            const void* src, VkDeviceSize bytes,
                            GpuStageScope* stage) {
  VKC_TRY(check(usable()));
  if (bytes == 0) return {};
  if (src == nullptr) {
    return check(Status::invalid_argument("CommandBatch::upload: src is null"));
  }
  if (bytes > kMaxInlineUpload || offset % 4 != 0 || bytes % 4 != 0) {
    VKC_ASSIGN(void* staging, reserve_upload(dst, offset, bytes, stage));
    std::memcpy(staging, src, static_cast<std::size_t>(bytes));
    return {};
  }
  // Inline: the bytes ride in the command buffer, and no staging exists.
  VKC_TRY(check(in_range(dst, offset, bytes, "upload")));
  VKC_TRY(check(has_usage(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "upload needs a TRANSFER_DST buffer")));
  Op op;
  op.kind = Kind::Update;
  op.dst = dst.handle();
  op.dst_offset = offset;
  op.bytes = bytes;
  const auto* from = static_cast<const unsigned char*>(src);
  op.data.assign(from, from + bytes);
  if (stage != nullptr) op.timing = stage->tag();
  ops_.push_back(std::move(op));
  return {};
}

Result<void*> CommandBatch::reserve_upload(const Buffer& dst,
                                           VkDeviceSize offset,
                                           VkDeviceSize bytes,
                                           GpuStageScope* stage) {
  VKC_TRY(check(usable()));
  if (bytes == 0) {
    return check(
        Status::invalid_argument("CommandBatch::reserve_upload: bytes is 0"));
  }
  VKC_TRY(check(in_range(dst, offset, bytes, "upload")));
  VKC_TRY(check(has_usage(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "upload needs a TRANSFER_DST buffer")));
  Result<const Buffer*> staged = this->stage(bytes, /*upload=*/true);
  if (!staged) return check(staged.status());
  Op op;
  op.kind = Kind::Copy;
  op.src = (*staged)->handle();
  op.dst = dst.handle();
  op.dst_offset = offset;
  op.bytes = bytes;
  op.staged = true;
  if (stage != nullptr) op.timing = stage->tag();
  ops_.push_back(std::move(op));
  return (*staged)->mapped();
}

Status CommandBatch::fill(const Buffer& dst, VkDeviceSize offset,
                          VkDeviceSize bytes, std::uint32_t value) {
  VKC_TRY(check(usable()));
  if (bytes == 0) return {};
  if (offset % 4 != 0 || bytes % 4 != 0) {
    return check(Status::invalid_argument(
        "CommandBatch::fill: offset and size must be multiples of 4"));
  }
  VKC_TRY(check(in_range(dst, offset, bytes, "fill")));
  VKC_TRY(check(has_usage(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "fill needs a TRANSFER_DST buffer")));
  Op op;
  op.kind = Kind::Fill;
  op.dst = dst.handle();
  op.dst_offset = offset;
  op.bytes = bytes;
  op.value = value;
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::zero(const Buffer& dst, VkDeviceSize offset,
                          VkDeviceSize bytes) {
  VKC_TRY(check(usable()));
  if (bytes == 0) return {};
  VKC_TRY(check(in_range(dst, offset, bytes, "zero")));
  VKC_TRY(check(has_usage(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "zero needs a TRANSFER_DST buffer")));
  const VkDeviceSize end = offset + bytes;
  const VkDeviceSize first_word =
      std::min((offset + 3) & ~VkDeviceSize{3}, end);
  const VkDeviceSize last_word = std::max(end & ~VkDeviceSize{3}, first_word);
  VKC_TRY(zero_edge(dst, offset, first_word - offset));
  VKC_TRY(fill(dst, first_word, last_word - first_word, 0U));
  return zero_edge(dst, last_word, end - last_word);
}

// Every edge copies from one word of zeros, staged on the first: zeroing
// thousands of scattered blocks allocates once, not twice a block. No command
// writes that word, so the copies join a run like any staged upload.
Status CommandBatch::zero_edge(const Buffer& dst, VkDeviceSize offset,
                               VkDeviceSize bytes) {
  if (bytes == 0) return {};
  if (zeros_ == VK_NULL_HANDLE) {
    Result<const Buffer*> staged = stage(4, /*upload=*/true);
    if (!staged) return check(staged.status());
    std::memset((*staged)->mapped(), 0, 4);
    zeros_ = (*staged)->handle();
  }
  Op op;
  op.kind = Kind::Copy;
  op.src = zeros_;
  op.dst = dst.handle();
  op.dst_offset = offset;
  op.bytes = bytes;
  op.staged = true;
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::copy(const Buffer& src, VkDeviceSize src_offset,
                          const Buffer& dst, VkDeviceSize dst_offset,
                          VkDeviceSize bytes, GpuStageScope* stage) {
  VKC_TRY(check(usable()));
  if (bytes == 0) return {};
  VKC_TRY(check(in_range(src, src_offset, bytes, "copy source")));
  VKC_TRY(check(in_range(dst, dst_offset, bytes, "copy destination")));
  VKC_TRY(check(has_usage(src, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          "copy needs a TRANSFER_SRC source")));
  VKC_TRY(check(has_usage(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "copy needs a TRANSFER_DST destination")));
  // vkCmdCopyBuffer forbids overlapping regions of one buffer.
  if (src.handle() == dst.handle() && src_offset < dst_offset + bytes &&
      dst_offset < src_offset + bytes) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: the ranges overlap in one buffer"));
  }
  Op op;
  op.kind = Kind::Copy;
  op.src = src.handle();
  op.src_offset = src_offset;
  op.dst = dst.handle();
  op.dst_offset = dst_offset;
  op.bytes = bytes;
  if (stage != nullptr) op.timing = stage->tag();
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::copy(const Image& src, std::uint32_t width,
                          std::uint32_t height, const Buffer& dst,
                          VkDeviceSize dst_offset, GpuStageScope* stage) {
  VKC_TRY(check(usable()));
  if (!src.valid()) {
    return check(
        Status::invalid_argument("CommandBatch::copy: the image is empty"));
  }
  const VkDeviceSize texel = copyable_texel_bytes(src.format());
  if (texel == 0) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: copies uncompressed 8-, 16- and 32-bit-channel "
        "color images only"));
  }
  if (src.samples() != VK_SAMPLE_COUNT_1_BIT) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: a multisampled image cannot be copied"));
  }
  if (width == 0 || height == 0 || width > src.width() ||
      height > src.height()) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: the region is empty or past the image"));
  }
  if (src.layout() != VK_IMAGE_LAYOUT_GENERAL &&
      src.layout() != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: the image is in a layout a copy cannot read"));
  }
  if ((src.usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: copy needs a TRANSFER_SRC image"));
  }
  // VUID-vkCmdCopyImageToBuffer-dstImage-07975 / bufferOffset-07737.
  if (dst_offset % 4 != 0 || dst_offset % texel != 0) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: the destination offset is not a multiple of 4 "
        "and of the texel size"));
  }
  // The image's size is its maker's word, so the bytes may not fit 64 bits.
  if (VkDeviceSize{width} * height > ~VkDeviceSize{0} / texel) {
    return check(Status::invalid_argument(
        "CommandBatch::copy: the region is past any buffer"));
  }
  const VkDeviceSize bytes = VkDeviceSize{width} * height * texel;
  VKC_TRY(check(in_range(dst, dst_offset, bytes, "copy destination")));
  VKC_TRY(check(has_usage(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          "copy needs a TRANSFER_DST destination")));
  Op op;
  op.kind = Kind::ImageCopy;
  op.image = src.handle();
  op.image_layout = src.layout();
  op.width = width;
  op.height = height;
  op.dst = dst.handle();
  op.dst_offset = dst_offset;
  op.bytes = bytes;
  if (stage != nullptr) op.timing = stage->tag();
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::acquire(const Buffer& buffer, std::uint32_t from) {
  return transfer(Kind::Acquire, buffer, from);
}

Status CommandBatch::release(const Buffer& buffer, std::uint32_t to) {
  return transfer(Kind::Release, buffer, to);
}

Status CommandBatch::transfer(Kind kind, const Buffer& buffer,
                              std::uint32_t family) {
  const std::string caller =
      kind == Kind::Acquire ? "CommandBatch::acquire" : "CommandBatch::release";
  VKC_TRY(check(usable()));
  if (!buffer.valid()) {
    return check(Status::invalid_argument(caller + ": the buffer is empty"));
  }
  const std::uint32_t own = device_->queue_family();
  const bool external = family == VK_QUEUE_FAMILY_EXTERNAL;
  if (!external && family != VK_QUEUE_FAMILY_IGNORED &&
      family >= device_->caps().queue_families().size()) {
    return check(Status::invalid_argument(caller + ": queue family " +
                                          std::to_string(family) +
                                          " is not one of the device's"));
  }
  const bool concurrent = buffer.sharing_mode() == VK_SHARING_MODE_CONCURRENT;
  if (family == VK_QUEUE_FAMILY_IGNORED || family == own ||
      (concurrent && !external)) {
    return {};
  }
  // A CONCURRENT buffer moves between outside Vulkan and every family at
  // once, which Vulkan spells with this side's family ignored.
  const std::uint32_t ours = concurrent ? VK_QUEUE_FAMILY_IGNORED : own;
  Op op;
  op.kind = kind;
  op.dst = buffer.handle();
  op.value = kind == Kind::Acquire ? family : ours;
  op.to_family = kind == Kind::Acquire ? ours : family;
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::check_dispatch(const ComputeKernel& kernel,
                                    const void* push, std::uint32_t push_size) {
  if (!kernel.valid()) {
    return Status::invalid_argument("CommandBatch: the kernel is not built");
  }
  if (push_size > 0 && push == nullptr) {
    return Status::invalid_argument(
        "CommandBatch: push is null with a non-zero push_size");
  }
  if (push_size % 4 != 0 || push_size > kernel.push_bytes) {
    return Status::invalid_argument(
        "CommandBatch: push_size is not a multiple of 4 or overruns the "
        "kernel's push-constant range");
  }
  return {};
}

CommandBatch::Op CommandBatch::dispatch_op(Kind kind,
                                           const ComputeKernel& kernel,
                                           const DescriptorSet& set,
                                           const void* push,
                                           std::uint32_t push_size) {
  Op op;
  op.kind = kind;
  op.pipeline = kernel.pipeline.handle();
  op.pipeline_layout = kernel.pipeline.layout();
  op.name = kernel.name;
  op.set = set;
  op.set_handle = set.handle();
  op.set_writes = set.writes();
  if (push_size > 0) {
    const auto* bytes = static_cast<const unsigned char*>(push);
    op.data.assign(bytes, bytes + push_size);
  }
  return op;
}

Status CommandBatch::dispatch(const ComputeKernel& kernel, const void* push,
                              std::uint32_t push_size, std::uint32_t groups,
                              std::uint32_t max_groups, GpuStageScope* stage) {
  return dispatch(kernel, kernel.set, push, push_size, groups, max_groups,
                  stage);
}

Status CommandBatch::dispatch(const ComputeKernel& kernel,
                              const DescriptorSet& set, const void* push,
                              std::uint32_t push_size, std::uint32_t groups,
                              std::uint32_t max_groups, GpuStageScope* stage) {
  VKC_TRY(check(usable()));
  if ((device_->queue_flags() & VK_QUEUE_COMPUTE_BIT) == 0) {
    return check(
        Status::unsupported("CommandBatch::dispatch: queue cannot compute"));
  }
  VKC_TRY(check(check_dispatch(kernel, push, push_size)));
  if (!set.valid()) {
    return check(
        Status::invalid_argument("CommandBatch::dispatch: the set is empty"));
  }
  if (groups > max_groups) {
    return check(Status::invalid_argument(
        "CommandBatch::dispatch: workgroup count exceeds the device's "
        "maxComputeWorkGroupCount[0]"));
  }
  Op op = dispatch_op(Kind::Dispatch, kernel, set, push, push_size);
  op.value = groups;
  if (stage != nullptr) op.timing = stage->tag();
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::dispatch_indirect(const ComputeKernel& kernel,
                                       const void* push,
                                       std::uint32_t push_size,
                                       const Buffer& args, VkDeviceSize offset,
                                       GpuStageScope* stage) {
  VKC_TRY(check(usable()));
  if ((device_->queue_flags() & VK_QUEUE_COMPUTE_BIT) == 0) {
    return check(Status::unsupported(
        "CommandBatch::dispatch_indirect: queue cannot compute"));
  }
  VKC_TRY(check(check_dispatch(kernel, push, push_size)));
  if (offset % 4 != 0) {
    return check(Status::invalid_argument(
        "CommandBatch::dispatch_indirect: offset must be a multiple of 4"));
  }
  VKC_TRY(check(in_range(args, offset, sizeof(VkDispatchIndirectCommand),
                         "indirect command")));
  VKC_TRY(check(has_usage(args, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                          "dispatch_indirect needs an INDIRECT_BUFFER")));
  Op op =
      dispatch_op(Kind::DispatchIndirect, kernel, kernel.set, push, push_size);
  op.src = args.handle();
  op.src_offset = offset;
  if (stage != nullptr) op.timing = stage->tag();
  ops_.push_back(std::move(op));
  return {};
}

Status CommandBatch::readback(const Buffer& src, VkDeviceSize offset,
                              VkDeviceSize bytes, void* dst) {
  VKC_TRY(check(usable()));
  if (bytes == 0) return {};
  if (dst == nullptr) {
    return check(
        Status::invalid_argument("CommandBatch::readback: dst is null"));
  }
  VKC_TRY(check(in_range(src, offset, bytes, "readback")));
  VKC_TRY(check(has_usage(src, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          "readback needs a TRANSFER_SRC buffer")));
  if (allocator_ == nullptr) {
    return check(Status::invalid_argument(
        "CommandBatch::readback: the batch has no allocator to read back "
        "through"));
  }
  Op op;
  op.kind = Kind::Readback;
  op.src = src.handle();
  op.src_offset = offset;
  op.bytes = bytes;
  op.host_dst = dst;
  ops_.push_back(std::move(op));
  return {};
}

// Two transfers run unordered unless they share a buffer one of them writes;
// a dispatch is always ordered. A readback's write is left out: its slice of
// the batch's own buffer is touched by no other command.
bool CommandBatch::needs_barrier(std::size_t first, std::size_t i) const {
  const auto dispatches = [](const Op& op) {
    return op.kind == Kind::Dispatch || op.kind == Kind::DispatchIndirect;
  };
  const auto written = [](const Op& op) -> VkBuffer {
    return op.kind == Kind::Readback ? VK_NULL_HANDLE : op.dst;
  };
  const auto touches = [](const Op& op, VkBuffer buffer) {
    return buffer != VK_NULL_HANDLE && (op.src == buffer || op.dst == buffer);
  };
  // An acquire is a barrier of its own, and orders what reads its buffer
  // after it; a release is recorded after everything, past the last barrier.
  const auto transfers = [](const Op& op) {
    return op.kind == Kind::Acquire || op.kind == Kind::Release;
  };
  const Op& b = ops_[i];
  if (transfers(b)) return false;
  // A fill or upload that starts past the end of the one before it, in the
  // same buffer, reads nothing another command writes and writes no byte the
  // run has: the run's first write there was checked against all of it. So
  // zeroing thousands of scattered blocks is one run, and costs no scan of it
  // per fill, and so is staging several images into one buffer. A staged
  // upload reads only its own staging, and a copy from another buffer reads
  // only that, so it joins the run too once nothing in the run writes its
  // source -- which, a run's commands writing only its buffer, is asked of
  // the joining copy alone. An image copy reads an image, which no command in
  // a batch writes.
  const auto plain = [](const Op& op) {
    return op.kind == Kind::Fill || op.kind == Kind::Update || op.staged ||
           (op.kind == Kind::Copy && op.src != op.dst) ||
           op.kind == Kind::ImageCopy;
  };
  if (i > first) {
    const Op& prev = ops_[i - 1];
    if (plain(prev) && plain(b) && prev.dst == b.dst &&
        b.dst_offset >= prev.dst_offset + prev.bytes) {
      bool source_written = false;
      if (b.kind == Kind::Copy && !b.staged) {
        for (std::size_t j = first; j < i && !source_written; ++j) {
          source_written = !transfers(ops_[j]) && written(ops_[j]) == b.src;
        }
      }
      if (!source_written) return false;
    }
  }
  for (std::size_t j = first; j < i; ++j) {
    const Op& a = ops_[j];
    if (transfers(a)) continue;
    if (dispatches(a) || dispatches(b) || touches(a, written(b)) ||
        touches(b, written(a))) {
      return true;
    }
  }
  return false;
}

void CommandBatch::record(VkCommandBuffer cmd,
                          std::vector<TimerRun>& runs) const {
  const detail::BatchScope writes =
      detail::batch_writes(device_->queue_flags());
  const detail::BatchScope commands =
      detail::batch_commands(device_->queue_flags());

  // Each timer's queries reset once, ahead of every span it opens here, not
  // once between every two commands.
  for (TimerRun& run : runs) {
    run.first = static_cast<std::uint32_t>(run.timer->count());
    run.timer->cmd_reset_ahead(cmd, run.ahead);
  }
  std::size_t first = 0;  // the first command since the last barrier
  for (std::size_t i = 0; i < ops_.size(); ++i) {
    const Op& op = ops_[i];
    if (op.kind == Kind::Release) continue;  // after everything, below
    // Each command sees every write before it -- the ordering one submit per
    // dispatch would give for free.
    if (i > 0 && needs_barrier(first, i)) {
      detail::batch_barrier(cmd, writes, commands);
      first = i;
    }
    // Only a dispatch is named, so the others open no region. The region
    // sits outside the span: a label may cost an encoder boundary (MoltenVK's
    // push/popDebugGroup), which a span must not measure.
    device_->begin_debug_label(cmd, op.name);
    const auto run = std::find_if(
        runs.begin(), runs.end(),
        [&op](const TimerRun& r) { return r.timer == op.timing.timer; });
    const std::uint32_t span =
        run != runs.end() ? run->timer->begin(cmd, op.timing, /*reset=*/false)
                          : GpuTimer::kNoSpan;
    switch (op.kind) {
      case Kind::Update:
        vkCmdUpdateBuffer(cmd, op.dst, op.dst_offset, op.bytes, op.data.data());
        break;
      case Kind::Copy:
      case Kind::Readback: {
        VkBufferCopy region{};
        region.srcOffset = op.src_offset;
        region.dstOffset = op.dst_offset;
        region.size = op.bytes;
        vkCmdCopyBuffer(cmd, op.src, op.dst, 1, &region);
        break;
      }
      case Kind::Fill:
        vkCmdFillBuffer(cmd, op.dst, op.dst_offset, op.bytes, op.value);
        break;
      case Kind::ImageCopy: {
        VkBufferImageCopy region{};
        region.bufferOffset = op.dst_offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {op.width, op.height, 1};
        vkCmdCopyImageToBuffer(cmd, op.image, op.image_layout, op.dst, 1,
                               &region);
        break;
      }
      case Kind::Acquire: {
        // The writer's release made its writes available, so this makes
        // them visible to everything after it, and waits on nothing before.
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.dstAccessMask = commands.access;
        b.srcQueueFamilyIndex = op.value;
        b.dstQueueFamilyIndex = op.to_family;
        b.buffer = op.dst;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             commands.stages, 0, 0, nullptr, 1, &b, 0, nullptr);
        break;
      }
      case Kind::Release:
        break;  // skipped above
      case Kind::Dispatch:
      case Kind::DispatchIndirect: {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, op.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                op.pipeline_layout, 0, 1, &op.set_handle, 0,
                                nullptr);
        if (!op.data.empty()) {
          vkCmdPushConstants(
              cmd, op.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              static_cast<std::uint32_t>(op.data.size()), op.data.data());
        }
        if (op.kind == Kind::Dispatch) {
          vkCmdDispatch(cmd, op.value, 1, 1);
        } else {
          vkCmdDispatchIndirect(cmd, op.src, op.src_offset);
        }
        break;
      }
    }
    if (span != GpuTimer::kNoSpan) {
      run->timer->end(cmd, span);
      ++run->count;
    }
    device_->end_debug_label(cmd, op.name);
  }
  // The final dependency covers all readers on this queue, including a
  // renderer's uniforms and storage buffers. Another queue still needs its
  // own synchronization, regardless of whether it belongs to the same family.
  detail::batch_final_barrier(cmd, device_->queue_flags());
  // Each release last, so every command, whenever recorded, ran on the
  // buffer first: it waits for them all and makes their writes available to
  // the family the buffer goes to, whose acquire makes them visible.
  for (const Op& op : ops_) {
    if (op.kind != Kind::Release) continue;
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = writes.access;
    b.srcQueueFamilyIndex = op.value;
    b.dstQueueFamilyIndex = op.to_family;
    b.buffer = op.dst;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, commands.stages,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                         &b, 0, nullptr);
  }
}

Status CommandBatch::submit() {
  if (device_ == nullptr) {
    return Status::invalid_argument("CommandBatch: the batch is moved-from");
  }
  if (submitted_) {
    return Status::invalid_argument("CommandBatch: already submitted");
  }
  submitted_ = true;
  if (!status_.ok()) return status_;
  if (ops_.empty()) return {};
  // The set is bound only now, so a write since its dispatch was recorded
  // would run that dispatch on the later binding, and a set whose pool is
  // gone would bind freed memory.
  for (const Op& op : ops_) {
    if (op.set_handle == VK_NULL_HANDLE) continue;  // not a dispatch
    if (!op.set.valid()) {
      return Status::invalid_argument(
          "CommandBatch::submit: a kernel's descriptor set was freed, with "
          "its pool, after its dispatch was recorded");
    }
    if (op.set.writes() != op.set_writes) {
      return Status::invalid_argument(
          "CommandBatch::submit: a kernel's descriptor set was rewritten "
          "after its dispatch was recorded");
    }
  }

  // Every readback lands in one host buffer, each at its own slice.
  VkDeviceSize readback_bytes = 0;
  for (Op& op : ops_) {
    if (op.kind != Kind::Readback) continue;
    op.dst_offset = readback_bytes;
    readback_bytes += op.bytes;
  }
  const Buffer* readbacks = nullptr;
  if (readback_bytes > 0) {
    VKC_ASSIGN(readbacks, stage(readback_bytes, /*upload=*/false));
    for (Op& op : ops_) {
      if (op.kind == Kind::Readback) op.dst = readbacks->handle();
    }
  }

  // Each timer the commands name, counted now: recording then allocates
  // nothing, so no exception leaves a span its command buffer never ran.
  std::vector<TimerRun> runs;
  for (const Op& op : ops_) {
    GpuTimer* timer = op.timing.timer;
    if (timer == nullptr || !timer->available()) continue;
    auto run =
        std::find_if(runs.begin(), runs.end(),
                     [timer](const TimerRun& r) { return r.timer == timer; });
    if (run == runs.end()) run = runs.insert(runs.end(), TimerRun{timer});
    ++run->ahead;
  }

  // Shared with the device, which keeps it past a failed wait, with the
  // command buffer it may still run, until it has waited for that. Moving the
  // vector leaves its buffers in place, so `readbacks` still points at one.
  const auto retained = std::make_shared<Retained>();
  retained->staging = std::exchange(staging_, {});
  for (const TimerRun& run : runs) {
    retained->queries.push_back(run.timer->keep_alive());
  }
  bool in_flight = false;
  const Status submitted = device_->submit_single_time(
      [&](VkCommandBuffer cmd) { record(cmd, runs); }, retained, &in_flight);
  for (const TimerRun& run : runs) {
    run.timer->settle(run.first, run.count, submitted, in_flight);
  }
  VKC_TRY(submitted);

  if (readbacks != nullptr) {
    const auto* base = static_cast<const unsigned char*>(readbacks->mapped());
    for (const Op& op : ops_) {
      if (op.kind != Kind::Readback) continue;
      std::memcpy(op.host_dst, base + op.dst_offset,
                  static_cast<std::size_t>(op.bytes));
    }
  }
  return {};
}

void CommandBatch::retain(Buffer buffer) {
  if (buffer.valid()) staging_.push_back(std::move(buffer));
}

}  // namespace volumetric_kit::core
