// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/external_memory.hpp"

#include <cstdint>
#include <optional>
#include <utility>

#include "memory_types.hpp"
#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/format.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

std::optional<std::uint32_t> find_memory_type(const Device& device,
                                              std::uint32_t type_bits,
                                              VkMemoryPropertyFlags required,
                                              VkMemoryPropertyFlags excluded) {
  return detail::first_memory_type(device.caps().memory_properties(), type_bits,
                                   required, excluded);
}

Result<ExportedBuffer> create_exported_buffer(const Device& device,
                                              Allocator& allocator,
                                              VkDeviceSize bytes) try {
  if (!allocator.valid()) {
    return Status::invalid_argument(
        "create_exported_buffer: allocator is moved-from");
  }
  // Checked before the buffer exists: memory from another device's allocator
  // cannot be bound to it, nor the buffer destroyed through that allocator.
  if (allocator.device_handle() != device.handle()) {
    return Status::invalid_argument(
        "create_exported_buffer: the allocator was made for another device");
  }
  if (!device.exports_memory()) {
    return Status::unsupported(
        "create_exported_buffer: the device does not export memory "
        "(VK_KHR_external_memory_fd)");
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
} catch (...) {
  return exception_status("create_exported_buffer",
                          Status::Code::InvalidArgument);
}

Result<ExportedImage> create_exported_image(const Device& device,
                                            Allocator& allocator,
                                            const ImageDesc& desc) try {
  constexpr const char* kCaller = "create_exported_image";
  if (!allocator.valid()) {
    return Status::invalid_argument(
        "create_exported_image: allocator is moved-from");
  }
  // As create_exported_buffer's, before the image exists.
  if (allocator.device_handle() != device.handle()) {
    return Status::invalid_argument(
        "create_exported_image: the allocator was made for another device");
  }
  if (!device.exports_memory()) {
    return Status::unsupported(
        "create_exported_image: the device does not export memory "
        "(VK_KHR_external_memory_fd)");
  }
  // What an exported image is, before create_image's checks: their advice
  // for a cube, a multisampled image or a multi-planar format's view would
  // lead to a description refused here.
  if (desc.type != VK_IMAGE_TYPE_2D || desc.cube || desc.mip_levels != 1 ||
      desc.array_layers != 1 || desc.samples != VK_SAMPLE_COUNT_1_BIT ||
      desc.tiling != VK_IMAGE_TILING_OPTIMAL) {
    return Status::invalid_argument(
        "create_exported_image: an exported image is 2D, of one mip, one "
        "layer and one sample, with optimal tiling");
  }
  if (format_needs_ycbcr_conversion(desc.format)) {
    return Status::invalid_argument(
        "create_exported_image: a multi-planar or 4:2:2 format; export each "
        "plane as an image of its own");
  }
  VKC_TRY(Allocator::check_image(desc, kCaller));
  constexpr VkExternalMemoryHandleTypeFlagBits kHandle =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  // Exporting an image is a property of its format, usage and tiling, and
  // the image is made only with a handle type the query names
  // (VUID-VkImageCreateInfo-pNext-00990). Zeroed, then set: handleType and
  // type have no zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceExternalImageFormatInfo external_query{};
  external_query.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  external_query.handleType = kHandle;
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkPhysicalDeviceImageFormatInfo2 query{};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  query.pNext = &external_query;
  query.format = desc.format;
  query.type = VK_IMAGE_TYPE_2D;
  query.tiling = VK_IMAGE_TILING_OPTIMAL;
  query.usage = desc.usage;
  VkExternalImageFormatProperties external_can{};
  external_can.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 can{};
  can.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  can.pNext = &external_can;
  const VkResult queried = vkGetPhysicalDeviceImageFormatProperties2(
      device.physical_device(), &query, &can);
  if (queried != VK_SUCCESS && queried != VK_ERROR_FORMAT_NOT_SUPPORTED) {
    return vk_error(queried, "vkGetPhysicalDeviceImageFormatProperties2");
  }
  const VkExternalMemoryProperties& handle =
      external_can.externalMemoryProperties;
  const VkExtent3D largest = can.imageFormatProperties.maxExtent;
  if (queried != VK_SUCCESS ||
      (handle.externalMemoryFeatures &
       VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0 ||
      (handle.compatibleHandleTypes & kHandle) == 0 ||
      desc.extent.width > largest.width ||
      desc.extent.height > largest.height) {
    return Status::unsupported(
        "create_exported_image: the device cannot export an image of this "
        "format, usage and extent as a file descriptor");
  }

  VkExternalMemoryImageCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  external.handleTypes = kHandle;
  // Placed, budgeted and dedicated as an exported buffer's memory is.
  VkExportMemoryAllocateInfo exported{};
  exported.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exported.handleTypes = kHandle;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize memory_size = 0;
  VKC_ASSIGN(Image made,
             allocator.make_image(desc, kCaller, &external, &exported, &memory,
                                  &memory_size));
  int fd = -1;
  const VkResult got = device.memory_fd(memory, &fd);
  if (got != VK_SUCCESS) return vk_error(got, "vkGetMemoryFdKHR");

  ExportedImage out;
  out.image = std::move(made);
  out.fd = UniqueFd(fd);
  out.memory_size = memory_size;
  return out;
} catch (...) {
  return exception_status("create_exported_image",
                          Status::Code::InvalidArgument);
}

}  // namespace volumetric_kit::core
