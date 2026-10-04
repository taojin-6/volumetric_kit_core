// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/allocator.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <vk_mem_alloc.h>

#include "queue_families.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

// The handles allocator.hpp keeps out. Freed in ~Impl, not ~Allocator, so the
// Allocator's moves default correctly; shared, because every buffer and image
// holds a reference and frees through it. The cost is one atomic per resource
// create and destroy, beside a device allocation.
struct Allocator::Impl {
  VmaAllocator allocator = nullptr;
  VkDevice device = VK_NULL_HANDLE;
  // The device's queue-family count, so a sharing list naming a family the
  // device lacks is refused: Vulkan offers no way to ask afterwards.
  std::uint32_t queue_family_count = 0;

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() {
    if (allocator != nullptr) vmaDestroyAllocator(allocator);
  }
};

namespace {

VmaMemoryUsage to_vma_usage(MemoryUsage memory) {
  switch (memory) {
    case MemoryUsage::DeviceLocal:
      return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryUsage::HostVisible:
      return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    case MemoryUsage::Auto:
      break;
  }
  return VMA_MEMORY_USAGE_AUTO;
}

// The sharing a resource gets from its queue-family list: exclusive for none
// or one distinct family, concurrent over two or more.
struct Sharing {
  VkSharingMode mode = VK_SHARING_MODE_EXCLUSIVE;
  std::uint32_t families[BufferDesc::kMaxQueueFamilies]{};
  std::uint32_t count = 0;
};

Result<Sharing> sharing_for(const std::uint32_t* families, std::uint32_t count,
                            std::uint32_t device_family_count,
                            const char* caller) {
  if (families == nullptr && count != 0) {
    return Status::invalid_argument(
        std::string(caller) +
        ": queue_family_count is non-zero but queue_families is null");
  }
  Sharing sharing;
  // Distinct families decide the mode, not the count passed: CONCURRENT must
  // name at least two, all unique, so a caller passing its compute and render
  // families unconditionally is malformed wherever they are one family.
  sharing.count = detail::distinct_queue_families(
      families, count, sharing.families, BufferDesc::kMaxQueueFamilies);
  if (sharing.count > BufferDesc::kMaxQueueFamilies) {
    return Status::invalid_argument(
        std::string(caller) +
        ": more than BufferDesc::kMaxQueueFamilies distinct queue families");
  }
  // An index naming no family is undefined under CONCURRENT
  // (VUID-VkBufferCreateInfo-sharingMode-01419), and VMA still succeeds, so
  // with layers off nothing reports it. It is how an app that hardcodes its
  // compute and render families -- valid on Apple's four -- breaks on a
  // single-family driver such as lavapipe.
  for (std::uint32_t i = 0; i < sharing.count; ++i) {
    if (sharing.families[i] >= device_family_count) {
      return Status::invalid_argument(
          std::string(caller) +
          ": queue_families names a family the device does not have");
    }
  }
  if (sharing.count > 1) sharing.mode = VK_SHARING_MODE_CONCURRENT;
  return sharing;
}

MemoryInfo memory_info_of(VmaAllocator allocator, std::uint32_t type_index) {
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(allocator, &props);
  const VkMemoryType& type = props->memoryTypes[type_index];
  return MemoryInfo{type.propertyFlags, type_index, type.heapIndex};
}

VkImageViewType view_type_for(VkImageType type, std::uint32_t layers,
                              bool cube) {
  if (cube) return VK_IMAGE_VIEW_TYPE_CUBE;
  const bool arrayed = layers > 1;
  switch (type) {
    case VK_IMAGE_TYPE_1D:
      return arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case VK_IMAGE_TYPE_3D:
      return VK_IMAGE_VIEW_TYPE_3D;
    default:
      return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  }
}

VkImageAspectFlags aspect_for(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      // A view of a combined depth/stencil image samples depth.
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
      return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

Status check_image_desc(const ImageDesc& desc) {
  if (desc.extent.width == 0 || desc.extent.height == 0) {
    return Status::invalid_argument("create_image: extent must be non-zero");
  }
  if (desc.usage == 0) {
    return Status::invalid_argument("create_image: usage is zero");
  }
  if (desc.format == VK_FORMAT_UNDEFINED) {
    return Status::invalid_argument("create_image: format is undefined");
  }
  if (desc.depth == 0 || desc.mip_levels == 0 || desc.array_layers == 0) {
    return Status::invalid_argument(
        "create_image: depth, mip_levels and array_layers must be non-zero");
  }
  if (desc.depth > 1 && desc.type != VK_IMAGE_TYPE_3D) {
    return Status::invalid_argument(
        "create_image: depth above 1 needs VK_IMAGE_TYPE_3D");
  }
  // VUID-VkImageCreateInfo-imageType-00956.
  if (desc.type == VK_IMAGE_TYPE_1D && desc.extent.height != 1) {
    return Status::invalid_argument(
        "create_image: a 1D image has extent.height 1");
  }
  if (desc.type == VK_IMAGE_TYPE_3D && desc.array_layers != 1) {
    return Status::invalid_argument(
        "create_image: a 3D image cannot be arrayed");
  }
  // VUID-VkImageCreateInfo-flags-00949, -imageType-00954, -samples-02257:
  // checked here, so a malformed cube reads as a domain error rather than an
  // opaque VkResult from vmaCreateImage.
  if (desc.cube && (desc.type != VK_IMAGE_TYPE_2D || desc.array_layers != 6 ||
                    desc.extent.width != desc.extent.height ||
                    desc.samples != VK_SAMPLE_COUNT_1_BIT)) {
    return Status::invalid_argument(
        "create_image: a cube is a single-sample, square 2D image of six "
        "layers");
  }
  // VUID-VkImageCreateInfo-samples-02257 / -02258.
  if (desc.samples != VK_SAMPLE_COUNT_1_BIT &&
      (desc.type != VK_IMAGE_TYPE_2D ||
       desc.tiling != VK_IMAGE_TILING_OPTIMAL || desc.mip_levels != 1)) {
    return Status::invalid_argument(
        "create_image: a multisampled image is 2D, optimal-tiling and "
        "single-mip");
  }
  if (desc.memory == MemoryUsage::HostVisible) {
    return Status::invalid_argument(
        "create_image: an image has no host accessor; copy it into a "
        "host-visible buffer to read it back");
  }
  // VUID-VkImageViewCreateInfo-image-04441.
  constexpr VkImageUsageFlags kViewCompatible =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  if (desc.with_view && (desc.usage & kViewCompatible) == 0) {
    return Status::invalid_argument(
        "create_image: a view needs a view-compatible usage (sampled, "
        "storage, or an attachment); clear with_view for a transfer-only "
        "image");
  }
  return {};
}

}  // namespace

Status check_queue_family_count(std::uint32_t count, const char* caller) {
  if (count > BufferDesc::kMaxQueueFamilies) {
    return Status::invalid_argument(
        std::string(caller) + ": queue_family_count must be 0.." +
        std::to_string(BufferDesc::kMaxQueueFamilies) + " (got " +
        std::to_string(count) + ")");
  }
  return {};
}

Result<Allocator> Allocator::create(VkInstance instance, const Device& device) {
  if (instance == VK_NULL_HANDLE) {
    return Status::invalid_argument("Allocator::create: instance is null");
  }
  if (device.handle() == VK_NULL_HANDLE ||
      device.physical_device() == VK_NULL_HANDLE) {
    return Status::invalid_argument("Allocator::create: device is empty");
  }

  // VMA may use Vulkan 1.1's features; the family needs none past them, and
  // the ceiling avoids 1.2+ entry points MoltenVK's loader path routes
  // differently. As recon and gfx did. The device's usable version already
  // bounds the instance's, so VMA never asks for more than either allows.
  const std::uint32_t usable = device.caps().api_version();

  // With VMA_STATIC_VULKAN_FUNCTIONS (vma_impl.cpp) VMA resolves entry points
  // against the linked loader; the two seeds keep it happy across VMA builds.
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info{};
  info.instance = instance;
  info.physicalDevice = device.physical_device();
  info.device = device.handle();
  info.vulkanApiVersion =
      usable >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
  info.pVulkanFunctions = &functions;

  // Never EXTERNALLY_SYNCHRONIZED: several threads allocate from one
  // allocator.
  auto impl = std::make_shared<Impl>();
  VKC_VK_TRY(vmaCreateAllocator(&info, &impl->allocator));
  impl->device = device.handle();
  impl->queue_family_count =
      static_cast<std::uint32_t>(device.caps().queue_families().size());

  Allocator allocator;
  allocator.impl_ = std::move(impl);
  return allocator;
}

Allocator::~Allocator() = default;
Allocator::Allocator(Allocator&& other) noexcept = default;
Allocator& Allocator::operator=(Allocator&& other) noexcept = default;

Result<Buffer> Allocator::create_buffer(const BufferDesc& desc) {
  if (impl_ == nullptr) {
    return Status::invalid_argument("create_buffer: allocator is moved-from");
  }
  if (desc.size == 0) {
    return Status::invalid_argument("create_buffer: size is zero");
  }
  if (desc.usage == 0) {
    return Status::invalid_argument("create_buffer: usage is zero");
  }
  if (desc.mapped && desc.memory == MemoryUsage::DeviceLocal) {
    return Status::invalid_argument(
        "create_buffer: a device-local buffer is never mapped; stage through "
        "a host-visible one");
  }
  if (desc.memory == MemoryUsage::HostVisible && !desc.mapped) {
    return Status::invalid_argument(
        "create_buffer: a host-visible buffer must be mapped (there is no "
        "separate map); set mapped");
  }
  VKC_ASSIGN(const Sharing sharing,
             sharing_for(desc.queue_families, desc.queue_family_count,
                         impl_->queue_family_count, "create_buffer"));

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = desc.size;
  buffer_info.usage = desc.usage;
  buffer_info.sharingMode = sharing.mode;
  if (sharing.mode == VK_SHARING_MODE_CONCURRENT) {
    buffer_info.queueFamilyIndexCount = sharing.count;
    buffer_info.pQueueFamilyIndices = sharing.families;
  }

  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = to_vma_usage(desc.memory);
  if (desc.memory == MemoryUsage::DeviceLocal) {
    // A preference alone lets VMA spill bulk kernel data into a non-local
    // heap; require residency. On unified memory the chosen type may also be
    // host-visible.
    alloc_info.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }
  if (desc.mapped) {
    alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_info.flags |=
        desc.host_access == HostAccess::SequentialWrite
            ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    // Coherent, so mapped() is a plain pointer: writes need no flush.
    alloc_info.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }

  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo out{};
  VKC_VK_TRY(vmaCreateBuffer(impl_->allocator, &buffer_info, &alloc_info,
                             &buffer, &allocation, &out));
  if (desc.mapped && out.pMappedData == nullptr) {
    vmaDestroyBuffer(impl_->allocator, buffer, allocation);
    return vk_error(VK_ERROR_MEMORY_MAP_FAILED,
                    "create_buffer: mapping requested, and VMA returned no "
                    "mapped pointer");
  }
  // The deleter holds the Impl, which keeps the VmaAllocator alive for as
  // long as this buffer can free through it.
  return Buffer(
      buffer, desc.size, desc.usage, sharing.mode, out.pMappedData,
      [impl = impl_, buffer, allocation] {
        vmaDestroyBuffer(impl->allocator, buffer, allocation);
      },
      memory_info_of(impl_->allocator, out.memoryType));
}

Result<Image> Allocator::create_image(const ImageDesc& desc) {
  if (impl_ == nullptr) {
    return Status::invalid_argument("create_image: allocator is moved-from");
  }
  VKC_TRY(check_image_desc(desc));
  VKC_ASSIGN(const Sharing sharing,
             sharing_for(desc.queue_families, desc.queue_family_count,
                         impl_->queue_family_count, "create_image"));

  // Zeroed, then every field set below -- samples included, which has no
  // zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  if (desc.cube) image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  image_info.imageType = desc.type;
  image_info.format = desc.format;
  image_info.extent = {desc.extent.width, desc.extent.height, desc.depth};
  image_info.mipLevels = desc.mip_levels;
  image_info.arrayLayers = desc.array_layers;
  image_info.samples = desc.samples;
  image_info.tiling = desc.tiling;
  image_info.usage = desc.usage;
  image_info.sharingMode = sharing.mode;
  if (sharing.mode == VK_SHARING_MODE_CONCURRENT) {
    image_info.queueFamilyIndexCount = sharing.count;
    image_info.pQueueFamilyIndices = sharing.families;
  }
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = to_vma_usage(desc.memory);
  if (desc.memory == MemoryUsage::DeviceLocal) {
    alloc_info.requiredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo out{};
  VKC_VK_TRY(vmaCreateImage(impl_->allocator, &image_info, &alloc_info, &image,
                            &allocation, &out));

  VkImageView view = VK_NULL_HANDLE;
  if (desc.with_view) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = view_type_for(desc.type, desc.array_layers, desc.cube);
    view_info.format = desc.format;
    view_info.subresourceRange.aspectMask = aspect_for(desc.format);
    view_info.subresourceRange.levelCount = desc.mip_levels;
    view_info.subresourceRange.layerCount = desc.array_layers;
    const VkResult viewed =
        vkCreateImageView(impl_->device, &view_info, nullptr, &view);
    if (viewed != VK_SUCCESS) {
      vmaDestroyImage(impl_->allocator, image, allocation);
      return vk_error(viewed, "vkCreateImageView");
    }
  }

  ImageInfo info;
  info.image = image;
  info.view = view;
  info.format = desc.format;
  info.extent = image_info.extent;
  info.mip_levels = desc.mip_levels;
  info.array_layers = desc.array_layers;
  info.usage = desc.usage;
  info.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.sharing = sharing.mode;
  info.memory = memory_info_of(impl_->allocator, out.memoryType);
  // The view goes first: it references the image.
  return Image(info, [impl = impl_, view, image, allocation] {
    if (view != VK_NULL_HANDLE) {
      vkDestroyImageView(impl->device, view, nullptr);
    }
    vmaDestroyImage(impl->allocator, image, allocation);
  });
}

MemoryStats Allocator::memory_stats() const {
  MemoryStats stats;
  if (impl_ == nullptr) return stats;
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(impl_->allocator, &props);
  if (props == nullptr) return stats;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
  vmaGetHeapBudgets(impl_->allocator, budgets);
  stats.heap_count = props->memoryHeapCount;
  for (std::uint32_t heap = 0; heap < stats.heap_count; ++heap) {
    stats.heaps[heap].usage_bytes = budgets[heap].usage;
    stats.heaps[heap].budget_bytes = budgets[heap].budget;
  }
  return stats;
}

}  // namespace volumetric_kit::core
