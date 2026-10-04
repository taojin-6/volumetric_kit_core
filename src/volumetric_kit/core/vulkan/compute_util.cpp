// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/compute_util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

VkDeviceSize max_storage_buffer_range(const Device& device) {
  return device.caps().limits().maxStorageBufferRange;
}

Status check_storage_buffer_range(const char* what, VkDeviceSize bytes,
                                  VkDeviceSize max_range) {
  if (bytes > max_range) {
    return Status::invalid_argument(
        std::string(what != nullptr ? what : "a binding") +
        " exceeds the device's maxStorageBufferRange");
  }
  return {};
}

Result<Buffer> mapped_storage_buffer(Allocator& allocator, VkDeviceSize bytes,
                                     HostAccess access,
                                     VkBufferUsageFlags extra_usage,
                                     const std::uint32_t* queue_families,
                                     std::uint32_t queue_family_count) {
  BufferDesc desc;
  desc.size = bytes;
  desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | extra_usage;
  desc.memory = MemoryUsage::DeviceMapped;
  desc.host_access = access;
  desc.queue_families = queue_families;
  desc.queue_family_count = queue_family_count;
  return allocator.create_buffer(desc);
}

Result<Buffer> upload_storage_buffer(Allocator& allocator, const void* src,
                                     VkDeviceSize bytes, HostAccess access) {
  if (src == nullptr) {
    return Status::invalid_argument("upload_storage_buffer: src is null");
  }
  VKC_ASSIGN(Buffer buffer, mapped_storage_buffer(allocator, bytes, access));
  std::memcpy(buffer.mapped(), src, static_cast<std::size_t>(bytes));
  return buffer;
}

Result<Buffer> device_storage_buffer(Allocator& allocator, VkDeviceSize bytes,
                                     VkBufferUsageFlags extra_usage,
                                     const std::uint32_t* queue_families,
                                     std::uint32_t queue_family_count) {
  BufferDesc desc;
  desc.size = bytes;
  desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra_usage;
  desc.memory = MemoryUsage::DeviceOnly;
  desc.queue_families = queue_families;
  desc.queue_family_count = queue_family_count;
  return allocator.create_buffer(desc);
}

Status ensure_device_scratch(const Device& device, Allocator& allocator,
                             Buffer& buffer, VkDeviceSize bytes,
                             VkDeviceSize max_range, const char* name,
                             CommandBatch* batch) {
  VKC_TRY(check_storage_buffer_range(name, bytes, max_range));
  if (buffer.size() >= bytes) return {};
  // The old one goes before its replacement is made, unless a batch has
  // commands on it, which keeps it until they have run.
  if (batch != nullptr) batch->retain(std::move(buffer));
  buffer = Buffer();
  VKC_ASSIGN(buffer, device_storage_buffer(
                         allocator, std::min(max_range, bytes + (bytes / 2))));
  device.set_object_name(VK_OBJECT_TYPE_BUFFER,
                         debug_object_handle(buffer.handle()), name);
  return {};
}

Status StorageInput::check(const char* what, VkDeviceSize bytes) const {
  const std::string named = what != nullptr ? what : "StorageInput";
  if (device_ == nullptr) {
    if (host_ == nullptr) return Status::invalid_argument(named + " is null");
    return {};
  }
  if (!device_->valid()) return Status::invalid_argument(named + " is empty");
  if ((device_->usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0) {
    return Status::invalid_argument(named + " is not a storage buffer");
  }
  if (device_->size() < bytes) {
    return Status::invalid_argument(
        named + " holds " + std::to_string(device_->size()) +
        " bytes; the binding reads " + std::to_string(bytes));
  }
  if (!device_->is_device_local()) {
    return Status::invalid_argument(
        named +
        " needs known device-local memory; pass host bytes, or upload into a "
        "device_storage_buffer");
  }
  return {};
}

Result<VkBuffer> StorageInput::buffer(CommandBatch& batch, Allocator& allocator,
                                      VkDeviceSize bytes,
                                      Buffer& upload) const {
  if (device_ != nullptr) {
    VKC_TRY(check("StorageInput", bytes));
    return device_->handle();
  }
  if (host_ == nullptr) {
    return Status::invalid_argument("StorageInput: the host bytes are null");
  }
  // Kept only when it is what device_storage_buffer makes, near enough: big
  // enough, device-local, bindable, and a copy's destination -- a
  // device-mapped buffer is device-local too, without TRANSFER_DST.
  constexpr VkBufferUsageFlags kUploadable =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (!upload.valid() || !upload.is_device_local() || upload.size() < bytes ||
      (upload.usage() & kUploadable) != kUploadable) {
    // What the batch recorded on the old one still runs on it.
    batch.retain(std::move(upload));
    upload = Buffer();
    // TODO: on unified memory, write the bytes into a DeviceMapped buffer
    // instead of staging a copy, once recon's iPad measurement settles which
    // inputs gain (DECISIONS.md, "Unified memory").
    VKC_ASSIGN(upload, device_storage_buffer(allocator, bytes));
  }
  VKC_TRY(batch.upload(upload, 0, host_, bytes));
  return upload.handle();
}

}  // namespace volumetric_kit::core
