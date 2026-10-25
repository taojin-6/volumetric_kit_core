// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/host_ring.hpp"

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "device_access.hpp"
#include "ring_slot.hpp"
#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<std::unique_ptr<HostRing>> HostRing::create(
    const Device& device, Allocator& allocator, const HostRingDesc& desc) try {
  if (desc.slot_bytes == 0 || desc.slots == 0) {
    return Status::invalid_argument(
        "HostRing::create: slot_bytes and slots must be non-zero");
  }
  if (device.handle() == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "HostRing::create: the device is moved-from");
  }
  if (!allocator.valid()) {
    return Status::invalid_argument(
        "HostRing::create: the allocator is moved-from");
  }
  std::unique_ptr<HostRing> ring(new HostRing(device, allocator, desc));
  for (const std::shared_ptr<detail::RingSlot>& slot : ring->slots_) {
    VKC_TRY(ring->place(*slot, desc.slot_bytes));
  }
  return ring;
} catch (...) {
  return exception_status("HostRing::create", Status::Code::OutOfMemory);
}

HostRing::HostRing(const Device& device, Allocator& allocator,
                   const HostRingDesc& desc)
    : device_(device),
      allocator_(allocator),
      name_(desc.name),
      in_place_(device.caps().device_mapped_cached()) {
  slots_.reserve(desc.slots);
  for (std::uint32_t i = 0; i < desc.slots; ++i) {
    slots_.push_back(std::make_shared<detail::RingSlot>());
  }
}

// A slot still leased keeps its buffers through its lease's `keep`.
HostRing::~HostRing() = default;

Status HostRing::place(detail::RingSlot& slot, VkDeviceSize bytes) {
  // The old buffers go first, so a grow never holds both; a failure leaves
  // the slot empty.
  slot.host = Buffer();
  slot.mirror = Buffer();
  BufferDesc desc;
  desc.size = bytes;
  desc.host_access = HostAccess::Random;  // the host reads results back
  if (in_place_) {
    desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    desc.memory = MemoryUsage::DeviceMapped;
    VKC_ASSIGN(slot.host, allocator_.create_buffer(desc));
  } else {
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    desc.memory = MemoryUsage::Staging;
    VKC_ASSIGN(Buffer host, allocator_.create_buffer(desc));
    VKC_ASSIGN(slot.mirror, device_storage_buffer(allocator_, bytes));
    slot.host = std::move(host);
  }
  if (name_ != nullptr) {
    for (const Buffer* buffer : {&slot.host, &slot.mirror}) {
      if (!buffer->valid()) continue;
      device_.set_object_name(VK_OBJECT_TYPE_BUFFER,
                              debug_object_handle(buffer->handle()), name_);
    }
  }
  detail::DeviceAccess::count_staging(device_);
  return {};
}

Result<HostSlot> HostRing::claim(VkDeviceSize bytes) try {
  if (bytes == 0) {
    return Status::invalid_argument("HostRing::claim: bytes is 0");
  }
  const std::shared_ptr<detail::RingSlot>* fit = nullptr;       // holds bytes
  const std::shared_ptr<detail::RingSlot>* smallest = nullptr;  // any size
  for (const std::shared_ptr<detail::RingSlot>& slot : slots_) {
    // Acquire: what its last holder did with the bytes happened before this.
    if (slot->held.load(std::memory_order_acquire)) continue;
    const VkDeviceSize size = slot->host.size();  // 0 after a failed grow
    if (smallest == nullptr || size < (*smallest)->host.size()) {
      smallest = &slot;
    }
    if (size >= bytes && (fit == nullptr || size < (*fit)->host.size())) {
      fit = &slot;
    }
  }
  if (smallest == nullptr) {
    return Status::invalid_argument(
        "HostRing::claim: all " + std::to_string(slots_.size()) + " slots" +
        (name_ != nullptr ? std::string(" of ") + name_ : std::string()) +
        " are held");
  }
  if (fit == nullptr) {
    // 1.5x, so a size that creeps up does not grow the slot every frame.
    const VkDeviceSize grown =
        bytes > ~VkDeviceSize{0} - (bytes / 2) ? bytes : bytes + (bytes / 2);
    VKC_TRY(place(**smallest, grown));
    fit = smallest;
  }
  const std::shared_ptr<detail::RingSlot>& chosen = *fit;
  chosen->held.store(true, std::memory_order_relaxed);
  // The lease. Its last copy clears `held` with a release store; `keep` lets
  // the slot outlive the ring. If the control block cannot be allocated, the
  // deleter runs at once and clears it again.
  HostSlot slot;
  slot.slot_ = std::shared_ptr<detail::RingSlot>(
      chosen.get(), [keep = chosen](detail::RingSlot* /*slot*/) {
        keep->held.store(false, std::memory_order_release);
      });
  slot.data_ = static_cast<std::uint8_t*>(chosen->host.mapped());
  slot.capacity_ = chosen->host.size();
  return slot;
} catch (...) {
  return exception_status("HostRing::claim", Status::Code::OutOfMemory);
}

}  // namespace volumetric_kit::core
