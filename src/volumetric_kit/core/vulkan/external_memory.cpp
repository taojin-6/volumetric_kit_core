// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/external_memory.hpp"

// The descriptor's close: POSIX's, or the CRT's on Windows, where no opaque
// descriptor is ever exported but the tier still builds.
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <cstdint>
#include <optional>
#include <utility>

#include "memory_types.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

void UniqueFd::reset(int fd) noexcept {
  if (fd_ >= 0 && fd_ != fd) {
#ifdef _WIN32
    _close(fd_);
#else
    close(fd_);
#endif
  }
  fd_ = fd;
}

std::optional<std::uint32_t> find_memory_type(const Device& device,
                                              std::uint32_t type_bits,
                                              VkMemoryPropertyFlags required,
                                              VkMemoryPropertyFlags excluded) {
  const VkPhysicalDeviceMemoryProperties& memory =
      device.caps().memory_properties();
  const std::uint32_t allowed =
      detail::memory_types_with(memory, required, excluded) & type_bits;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((allowed & (1U << i)) != 0) return i;
  }
  return std::nullopt;
}

Result<ExportedBuffer> create_exported_buffer(const Device& device,
                                              Allocator& allocator,
                                              VkDeviceSize bytes) {
  if (!device.exports_memory()) {
    return Status::unsupported(
        "create_exported_buffer: the device does not export memory "
        "(VK_KHR_external_memory_fd)");
  }
  if (!allocator.valid()) {
    return Status::invalid_argument(
        "create_exported_buffer: allocator is moved-from");
  }
  if (bytes == 0) {
    return Status::invalid_argument("create_exported_buffer: size is zero");
  }
  constexpr VkExternalMemoryHandleTypeFlagBits kHandle =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  // The extension alone does not promise an exportable storage buffer.
  // Zeroed, then set: handleType has no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalBufferInfo query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
  query.usage = kUsage;
  query.handleType = kHandle;
  VkExternalBufferProperties can{};
  can.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
  vkGetPhysicalDeviceExternalBufferProperties(device.physical_device(), &query,
                                              &can);
  if ((can.externalMemoryProperties.externalMemoryFeatures &
       VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
    return Status::unsupported(
        "create_exported_buffer: the device cannot export a storage buffer "
        "as a file descriptor");
  }

  VkExternalMemoryBufferCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external.handleTypes = kHandle;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.pNext = &external;
  info.size = bytes;
  info.usage = kUsage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateBuffer(device.handle(), &info, nullptr, &buffer));

  // Placed, budgeted and dedicated by the allocator (DECISIONS.md, "Where
  // memory lives"), with the export chained to its allocation.
  VkExportMemoryAllocateInfo exported{};
  exported.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exported.handleTypes = kHandle;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize memory_size = 0;
  VKC_ASSIGN(Buffer made,
             allocator.bind_exported(buffer, bytes, kUsage, &exported, &memory,
                                     &memory_size));
  int fd = -1;
  const VkResult got = device.memory_fd(memory, &fd);
  if (got != VK_SUCCESS) return vk_error(got, "vkGetMemoryFdKHR");

  ExportedBuffer out;
  out.buffer = std::move(made);
  out.fd = UniqueFd(fd);
  out.memory_size = memory_size;
  return out;
}

}  // namespace volumetric_kit::core
