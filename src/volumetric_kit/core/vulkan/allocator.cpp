// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/allocator.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// VMA is header-only, and this is the one translation unit that instantiates
// it: here, in the allocator's own object, rather than in a file of its own.
// A static link that also pulls in another VMA implementation -- a sibling's,
// before it moves to this allocator -- then fails on duplicate symbols,
// instead of leaving this allocator to run the other copy, possibly built
// against other Vulkan headers and so other struct layouts. A shared core
// hides VMA's symbols (CXX_VISIBILITY_PRESET hidden). VMA never reaches a
// public header or a consumer.
//
// VMA_STATIC_VULKAN_FUNCTIONS resolves entry points against the link-time
// loader the tier already links (vulkan.hpp, included above through
// allocator.hpp, so the prototypes are in scope before VMA expands). Adopting
// volk for iOS and Android would switch this to the dynamic-functions path,
// here alone.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "allocation_budget.hpp"
#include "memory_types.hpp"
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
  // The heaps and types each placement is cut from (memory_types.hpp).
  VkPhysicalDeviceMemoryProperties memory{};
  bool dedicated_queries = false;

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() {
    if (allocator != nullptr) vmaDestroyAllocator(allocator);
  }
};

namespace {

// Core 1.1 exposes mandatory dedicated-allocation requirements. The legacy
// query remains valid for an adopted 1.0 device, on which VMA enables neither
// dedicated-allocation extension. A required dedicated allocation must skip
// the attempt to reuse a block, which VMA would reject as contradictory.
struct ResourceRequirements {
  VkMemoryRequirements memory{};
  bool dedicated = false;
};

ResourceRequirements buffer_requirements(VkDevice device, VkBuffer buffer,
                                         bool dedicated_queries) {
  ResourceRequirements result;
  if (!dedicated_queries) {
    vkGetBufferMemoryRequirements(device, buffer, &result.memory);
    return result;
  }
  VkMemoryDedicatedRequirements dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 requirements{};
  requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  requirements.pNext = &dedicated;
  VkBufferMemoryRequirementsInfo2 info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
  info.buffer = buffer;
  vkGetBufferMemoryRequirements2(device, &info, &requirements);
  result.memory = requirements.memoryRequirements;
  result.dedicated = dedicated.requiresDedicatedAllocation == VK_TRUE;
  return result;
}

ResourceRequirements image_requirements(VkDevice device, VkImage image,
                                        bool dedicated_queries) {
  ResourceRequirements result;
  if (!dedicated_queries) {
    vkGetImageMemoryRequirements(device, image, &result.memory);
    return result;
  }
  VkMemoryDedicatedRequirements dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 requirements{};
  requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  requirements.pNext = &dedicated;
  VkImageMemoryRequirementsInfo2 info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
  info.image = image;
  vkGetImageMemoryRequirements2(device, &info, &requirements);
  result.memory = requirements.memoryRequirements;
  result.dedicated = dedicated.requiresDedicatedAllocation == VK_TRUE;
  return result;
}

// The refusal for a resource no type of its placement suits.
Status unsuited(const char* caller, MemoryUsage memory) {
  switch (memory) {
    case MemoryUsage::DeviceOnly:
      return Status::unsupported(
          std::string(caller) +
          ": no device-only memory type suits the resource; the rest of the "
          "device-local memory is the BAR window");
    case MemoryUsage::DeviceMapped:
      return Status::unsupported(
          std::string(caller) +
          ": no device-local memory the host maps coherently (and cached, "
          "for HostAccess::Random) suits the buffer; make it DeviceOnly and "
          "upload through a CommandBatch, or read it back by one");
    case MemoryUsage::Staging:
      break;
  }
  return Status::unsupported(std::string(caller) +
                             ": no host memory suits the staging buffer");
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
  // compute and render families for one driver breaks on another with fewer
  // families.
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

// Whether a COLOR view of @p format must carry a sampler Y'CbCr conversion
// (VUID-VkImageViewCreateInfo-format-06415): the core formats the registry
// (vk.xml) gives a chroma attribute -- multi-planar, 4:2:2, and the RGBA
// 4PACK16 ones -- such as a decoder's NV12 picture. The one- and
// two-component R10X6 / R12X4 formats inside the 1.1 range need none.
bool needs_ycbcr_conversion(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R10X6_UNORM_PACK16:
    case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
    case VK_FORMAT_R12X4_UNORM_PACK16:
    case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
      return false;
    default:
      return (format >= VK_FORMAT_G8B8G8R8_422_UNORM &&
              format <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
             (format >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM &&
              format <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
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
  // VUID-VkImageViewCreateInfo-image-04441 allows a view for nearly every
  // usage but the transfers -- video, shading-rate and density-map usages
  // included -- so only a transfer-only image is refused here: a list of the
  // allowed bits would refuse the ones it missed.
  constexpr VkImageUsageFlags kTransferOnly =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (desc.with_view && (desc.usage & ~kTransferOnly) == 0) {
    return Status::invalid_argument(
        "create_image: a view needs a usage beyond transfer; clear with_view "
        "for a transfer-only image");
  }
  if (desc.with_view && needs_ycbcr_conversion(desc.format)) {
    return Status::invalid_argument(
        "create_image: a multi-planar or 4:2:2 format's view needs a sampler "
        "Y'CbCr conversion, which the default view cannot carry; clear "
        "with_view and make the view with one");
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

  // With VMA_STATIC_VULKAN_FUNCTIONS (above) VMA resolves entry points
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
  // The driver's own budget, where the device enabled the extension (Device
  // enables it where offered); VMA reads it through
  // vkGetPhysicalDeviceMemoryProperties2, core in 1.1. Otherwise VMA's
  // estimate: this allocator's usage against 80% of each heap.
  if (info.vulkanApiVersion >= VK_API_VERSION_1_1 &&
      device.extension_enabled(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
    info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  }

  // Never EXTERNALLY_SYNCHRONIZED: several threads allocate from one
  // allocator.
  auto impl = std::make_shared<Impl>();
  VKC_VK_TRY(vmaCreateAllocator(&info, &impl->allocator));
  impl->device = device.handle();
  impl->queue_family_count =
      static_cast<std::uint32_t>(device.caps().queue_families().size());
  impl->memory = device.caps().memory_properties();
  impl->dedicated_queries = info.vulkanApiVersion >= VK_API_VERSION_1_1;

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
  // VMA aborts on this bit (an assert) unless the allocator was made with
  // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, which needs the device's
  // bufferDeviceAddress feature; refused here, as recon's MarchingCubes did,
  // so the caller gets an error instead.
  //
  // TODO: allow it once DeviceRequirements can enable bufferDeviceAddress,
  // making the allocator with VMA's flag on a device that has it.
  if ((desc.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
    return Status::invalid_argument(
        "create_buffer: VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT is not "
        "supported -- the allocator does not enable buffer device addresses");
  }
  // The GPU reaches host memory only by a copy: a shader, vertex fetch or
  // indirect read of it would cross PCIe on every access on a discrete GPU.
  constexpr VkBufferUsageFlags kTransfer =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (desc.memory == MemoryUsage::Staging && (desc.usage & ~kTransfer) != 0) {
    return Status::invalid_argument(
        "create_buffer: a staging buffer is a copy's source or destination "
        "only; memory the GPU reads directly is DeviceOnly, or DeviceMapped "
        "when the host writes it");
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

  // The buffer first, so its placement is cut to the memory types its own
  // requirements allow, rather than found wanting by an allocation.
  VkBuffer buffer = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateBuffer(impl_->device, &buffer_info, nullptr, &buffer));
  const ResourceRequirements requirements =
      buffer_requirements(impl_->device, buffer, impl_->dedicated_queries);
  const VkMemoryRequirements& needs = requirements.memory;
  const std::uint32_t types = detail::placement_types(
      impl_->memory, desc.memory, desc.host_access, needs.memoryTypeBits);
  if (types == 0) {
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return unsuited("create_buffer", desc.memory);
  }
  // A device-mapped or staging buffer is mapped persistently, and its types
  // are coherent, so mapped() is a plain pointer: writes need no flush.
  const bool mapped = desc.memory != MemoryUsage::DeviceOnly;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo out{};
  const char* step = "vmaAllocateMemoryForBuffer";
  VkResult made = detail::allocate_with_budget(
      impl_->memory, needs, types, mapped, requirements.dedicated,
      [&](const VmaAllocationCreateInfo& info) {
        return vmaAllocateMemoryForBuffer(impl_->allocator, buffer, &info,
                                          &allocation, &out);
      },
      [&] { return memory_stats(); });
  if (made == VK_SUCCESS) {
    step = "vmaBindBufferMemory";
    made = vmaBindBufferMemory(impl_->allocator, allocation, buffer);
  }
  if (made != VK_SUCCESS) {
    vmaDestroyBuffer(impl_->allocator, buffer, allocation);
    return vk_error(made, step);
  }
  if (mapped && out.pMappedData == nullptr) {
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

  VkImage image = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateImage(impl_->device, &image_info, nullptr, &image));
  const ResourceRequirements requirements =
      image_requirements(impl_->device, image, impl_->dedicated_queries);
  const VkMemoryRequirements& needs = requirements.memory;
  // The host access is ignored for device-only memory.
  const std::uint32_t types = detail::placement_types(
      impl_->memory, MemoryUsage::DeviceOnly, HostAccess::SequentialWrite,
      needs.memoryTypeBits);
  if (types == 0) {
    vkDestroyImage(impl_->device, image, nullptr);
    return unsuited("create_image", MemoryUsage::DeviceOnly);
  }
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo out{};
  const char* step = "vmaAllocateMemoryForImage";
  VkResult made = detail::allocate_with_budget(
      impl_->memory, needs, types, /*mapped=*/false, requirements.dedicated,
      [&](const VmaAllocationCreateInfo& info) {
        return vmaAllocateMemoryForImage(impl_->allocator, image, &info,
                                         &allocation, &out);
      },
      [&] { return memory_stats(); });
  if (made == VK_SUCCESS) {
    step = "vmaBindImageMemory";
    made = vmaBindImageMemory(impl_->allocator, allocation, image);
  }
  if (made != VK_SUCCESS) {
    vmaDestroyImage(impl_->allocator, image, allocation);
    return vk_error(made, step);
  }

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
  info.flags = image_info.flags;
  info.type = desc.type;
  info.format = desc.format;
  info.extent = image_info.extent;
  info.mip_levels = desc.mip_levels;
  info.array_layers = desc.array_layers;
  info.samples = desc.samples;
  info.tiling = desc.tiling;
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

Result<Buffer> Allocator::bind_exported(VkBuffer buffer, VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        void* export_info,
                                        VkDeviceMemory* memory,
                                        VkDeviceSize* memory_size) {
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(impl_->device, buffer, &needs);
  // The host access is ignored for device-only memory.
  const std::uint32_t types = detail::placement_types(
      impl_->memory, MemoryUsage::DeviceOnly, HostAccess::SequentialWrite,
      needs.memoryTypeBits);
  if (types == 0) {
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return unsuited("create_exported_buffer", MemoryUsage::DeviceOnly);
  }
  // Dedicated, as an importer of an opaque descriptor maps the whole
  // allocation as one resource. VMA names the buffer only for one it creates
  // itself, so the chain does.
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = export_info;
  dedicated.buffer = buffer;
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo out{};
  const char* step = "vmaAllocateDedicatedMemory";
  VkResult made = detail::allocate_with_budget(
      impl_->memory, needs, types, /*mapped=*/false,
      /*dedicated_required=*/true,
      [&](const VmaAllocationCreateInfo& info) {
        return vmaAllocateDedicatedMemory(impl_->allocator, &needs, &info,
                                          &dedicated, &allocation, &out);
      },
      [&] { return memory_stats(); });
  if (made == VK_SUCCESS) {
    step = "vmaBindBufferMemory";
    made = vmaBindBufferMemory(impl_->allocator, allocation, buffer);
  }
  if (made != VK_SUCCESS) {
    vmaDestroyBuffer(impl_->allocator, buffer, allocation);
    return vk_error(made, step);
  }
  *memory = out.deviceMemory;
  *memory_size = out.size;
  return Buffer(
      buffer, size, usage, VK_SHARING_MODE_EXCLUSIVE, nullptr,
      [impl = impl_, buffer, allocation] {
        vmaDestroyBuffer(impl->allocator, buffer, allocation);
      },
      memory_info_of(impl_->allocator, out.memoryType));
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
    stats.heaps[heap].usage_bytes = budgets[heap].statistics.blockBytes;
    stats.heaps[heap].allocation_bytes =
        budgets[heap].statistics.allocationBytes;
    stats.heaps[heap].heap_usage_bytes = budgets[heap].usage;
    stats.heaps[heap].budget_bytes = budgets[heap].budget;
  }
  return stats;
}

}  // namespace volumetric_kit::core
