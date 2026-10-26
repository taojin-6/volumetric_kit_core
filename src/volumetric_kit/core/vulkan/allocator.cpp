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
// allocator.hpp, so the prototypes are in scope before VMA expands), or
// MoltenVK's static library on iOS. Adopting volk for Android would switch
// this to the dynamic-functions path, here alone.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "allocation_budget.hpp"
#include "memory_types.hpp"
#include "queue_families.hpp"
#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/format.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

namespace {

// What place() needs of an allocator: its Impl, which free functions cannot
// name, and the allocator itself for its memory_stats().
struct AllocatorView {
  const Allocator& owner;
  VkDevice device = VK_NULL_HANDLE;
  VmaAllocator vma = nullptr;
  const VkPhysicalDeviceMemoryProperties& memory;
  bool dedicated_queries = false;
};

}  // namespace

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

  AllocatorView view(const Allocator& owner) const {
    return AllocatorView{owner, device, allocator, memory, dedicated_queries};
  }

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() {
    if (allocator != nullptr) vmaDestroyAllocator(allocator);
  }
};

namespace {

// A resource to give memory: a buffer or an image (the other null) and, for
// an exported one, the dedicated-allocation chain its memory is made with.
// Not overloads on the two handle types, which are one integer type on a
// 32-bit target.
struct Resource {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkMemoryDedicatedAllocateInfo* exported = nullptr;  // VMA takes it non-const
};

// A resource's memory requirements, and whether the driver requires memory
// dedicated to it -- core 1.1's query. The legacy query remains for an
// adopted 1.0 device, on which VMA enables neither dedicated-allocation
// extension.
struct ResourceRequirements {
  VkMemoryRequirements memory{};
  bool dedicated = false;
};

ResourceRequirements requirements_of(VkDevice device, const Resource& resource,
                                     bool dedicated_queries) {
  ResourceRequirements result;
  if (!dedicated_queries) {
    if (resource.buffer != VK_NULL_HANDLE) {
      vkGetBufferMemoryRequirements(device, resource.buffer, &result.memory);
    } else {
      vkGetImageMemoryRequirements(device, resource.image, &result.memory);
    }
    return result;
  }
  VkMemoryDedicatedRequirements dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 requirements{};
  requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  requirements.pNext = &dedicated;
  if (resource.buffer != VK_NULL_HANDLE) {
    VkBufferMemoryRequirementsInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    info.buffer = resource.buffer;
    vkGetBufferMemoryRequirements2(device, &info, &requirements);
  } else {
    VkImageMemoryRequirementsInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    info.image = resource.image;
    vkGetImageMemoryRequirements2(device, &info, &requirements);
  }
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

// A resource's memory: its allocation, and where it landed.
struct Placed {
  VmaAllocation allocation = nullptr;
  VmaAllocationInfo info{};
};

// Own every completed step until the returned Buffer/Image has its deleter.
// In particular, constructing that std::function can allocate after placement
// and view creation have already succeeded.
struct OwnedResource {
  OwnedResource(const AllocatorView& owner, Resource handles) noexcept
      : device(owner.device), allocator(owner.vma), resource(handles) {}
  OwnedResource(const OwnedResource&) = delete;
  OwnedResource& operator=(const OwnedResource&) = delete;
  OwnedResource(OwnedResource&&) = delete;
  OwnedResource& operator=(OwnedResource&&) = delete;
  ~OwnedResource() {
    if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    if (resource.buffer != VK_NULL_HANDLE) {
      vmaDestroyBuffer(allocator, resource.buffer, placed.allocation);
    } else if (resource.image != VK_NULL_HANDLE) {
      vmaDestroyImage(allocator, resource.image, placed.allocation);
    }
  }
  void release() noexcept {
    resource = {};
    placed = {};
    view = VK_NULL_HANDLE;
  }

  VkDevice device;
  VmaAllocator allocator;
  Resource resource;
  Placed placed;
  VkImageView view = VK_NULL_HANDLE;
};

// Gives `resource` memory of placement `usage`: cut to the types its
// requirements allow (memory_types.hpp), allocated within budget
// (allocation_budget.hpp), bound, and mapped unless it is DeviceOnly. Its
// owner keeps partial placement on every return or exception; `caller`
// prefixes refusals of its own.
Status place(const AllocatorView& allocator, OwnedResource& owner,
             MemoryUsage usage, HostAccess access, const char* caller) {
  const Resource& resource = owner.resource;
  Placed& placed = owner.placed;
  const bool is_buffer = resource.buffer != VK_NULL_HANDLE;
  const ResourceRequirements requirements =
      requirements_of(allocator.device, resource, allocator.dedicated_queries);
  const std::uint32_t types = detail::placement_types(
      allocator.memory, usage, access, requirements.memory.memoryTypeBits);
  if (types == 0) {
    return unsuited(caller, usage);
  }
  // A device-mapped or staging buffer is mapped persistently, and its types
  // are coherent, so mapped() is a plain pointer: writes need no flush.
  const bool mapped = usage != MemoryUsage::DeviceOnly;
  const char* vma_call = "vmaAllocateDedicatedMemory";
  if (resource.exported == nullptr) {
    vma_call =
        is_buffer ? "vmaAllocateMemoryForBuffer" : "vmaAllocateMemoryForImage";
  }
  const detail::BudgetedAllocation made = detail::allocate_with_budget(
      allocator.memory, requirements.memory, types, mapped,
      requirements.dedicated || resource.exported != nullptr,
      [&](const VmaAllocationCreateInfo& info) {
        // An exported resource's memory is dedicated, as an importer of an
        // opaque descriptor maps the whole allocation as one resource. VMA
        // names the resource only for one it creates itself, so the chain
        // does.
        if (resource.exported != nullptr) {
          return vmaAllocateDedicatedMemory(allocator.vma, &requirements.memory,
                                            &info, resource.exported,
                                            &placed.allocation, &placed.info);
        }
        if (is_buffer) {
          return vmaAllocateMemoryForBuffer(allocator.vma, resource.buffer,
                                            &info, &placed.allocation,
                                            &placed.info);
        }
        return vmaAllocateMemoryForImage(allocator.vma, resource.image, &info,
                                         &placed.allocation, &placed.info);
      },
      [&] { return allocator.owner.memory_stats(); });
  if (made.result != VK_SUCCESS) {
    if (made.over_budget) {
      return vk_error(made.result, std::string(caller) +
                                       ": past every candidate heap's budget");
    }
    return vk_error(made.result, vma_call);
  }
  const VkResult bound =
      is_buffer ? vmaBindBufferMemory(allocator.vma, placed.allocation,
                                      resource.buffer)
                : vmaBindImageMemory(allocator.vma, placed.allocation,
                                     resource.image);
  if (bound != VK_SUCCESS) {
    return vk_error(bound,
                    is_buffer ? "vmaBindBufferMemory" : "vmaBindImageMemory");
  }
  if (mapped && placed.info.pMappedData == nullptr) {
    return vk_error(VK_ERROR_MEMORY_MAP_FAILED,
                    std::string(caller) +
                        ": mapping requested, and VMA returned no mapped "
                        "pointer");
  }
  return {};
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

Status check_image_desc(const ImageDesc& desc, const char* caller) {
  const auto refuse = [caller](const char* why) {
    return Status::invalid_argument(std::string(caller) + ": " + why);
  };
  if (desc.extent.width == 0 || desc.extent.height == 0) {
    return refuse("extent must be non-zero");
  }
  if (desc.usage == 0) return refuse("usage is zero");
  if (desc.format == VK_FORMAT_UNDEFINED) return refuse("format is undefined");
  if (desc.depth == 0 || desc.mip_levels == 0 || desc.array_layers == 0) {
    return refuse("depth, mip_levels and array_layers must be non-zero");
  }
  if (desc.depth > 1 && desc.type != VK_IMAGE_TYPE_3D) {
    return refuse("depth above 1 needs VK_IMAGE_TYPE_3D");
  }
  // VUID-VkImageCreateInfo-imageType-00956.
  if (desc.type == VK_IMAGE_TYPE_1D && desc.extent.height != 1) {
    return refuse("a 1D image has extent.height 1");
  }
  if (desc.type == VK_IMAGE_TYPE_3D && desc.array_layers != 1) {
    return refuse("a 3D image cannot be arrayed");
  }
  // VUID-VkImageCreateInfo-flags-00949, -imageType-00954, -samples-02257:
  // checked here, so a malformed cube reads as a domain error rather than an
  // opaque VkResult from vmaCreateImage.
  if (desc.cube && (desc.type != VK_IMAGE_TYPE_2D || desc.array_layers != 6 ||
                    desc.extent.width != desc.extent.height ||
                    desc.samples != VK_SAMPLE_COUNT_1_BIT)) {
    return refuse("a cube is a single-sample, square 2D image of six layers");
  }
  // VUID-VkImageCreateInfo-samples-02257 / -02258.
  if (desc.samples != VK_SAMPLE_COUNT_1_BIT &&
      (desc.type != VK_IMAGE_TYPE_2D ||
       desc.tiling != VK_IMAGE_TILING_OPTIMAL || desc.mip_levels != 1)) {
    return refuse("a multisampled image is 2D, optimal-tiling and single-mip");
  }
  // VUID-VkImageViewCreateInfo-image-04441 allows a view for nearly every
  // usage but the transfers -- video, shading-rate and density-map usages
  // included -- so only a transfer-only image is refused here: a list of the
  // allowed bits would refuse the ones it missed.
  constexpr VkImageUsageFlags kTransferOnly =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (desc.with_view && (desc.usage & ~kTransferOnly) == 0) {
    return refuse(
        "a view needs a usage beyond transfer; clear with_view for a "
        "transfer-only image");
  }
  if (desc.with_view && format_needs_ycbcr_conversion(desc.format)) {
    return refuse(
        "a multi-planar or 4:2:2 format's view needs a sampler Y'CbCr "
        "conversion, which the default view cannot carry; clear with_view and "
        "make the view with one");
  }
  return {};
}

}  // namespace

Status check_queue_family_count(std::uint32_t count, const char* caller) try {
  if (count > BufferDesc::kMaxQueueFamilies) {
    return Status::invalid_argument(
        std::string(caller) + ": queue_family_count must be 0.." +
        std::to_string(BufferDesc::kMaxQueueFamilies) + " (got " +
        std::to_string(count) + ")");
  }
  return {};
} catch (...) {
  return exception_status("check_queue_family_count",
                          Status::Code::InvalidArgument);
}

Result<Allocator> Allocator::create(VkInstance instance,
                                    const Device& device) try {
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
} catch (...) {
  return exception_status("Allocator::create", Status::Code::InvalidArgument);
}

Allocator::~Allocator() = default;
Allocator::Allocator(Allocator&& other) noexcept = default;
Allocator& Allocator::operator=(Allocator&& other) noexcept = default;

Result<Buffer> Allocator::create_buffer(const BufferDesc& desc) try {
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
  Resource resource;
  resource.buffer = buffer;
  OwnedResource owned(impl_->view(*this), resource);
  VKC_TRY(place(impl_->view(*this), owned, desc.memory, desc.host_access,
                "create_buffer"));
  const Placed& placed = owned.placed;
  // The deleter holds the Impl, which keeps the VmaAllocator alive for as
  // long as this buffer can free through it.
  Buffer result(
      buffer, desc.size, desc.usage, sharing.mode, placed.info.pMappedData,
      [impl = impl_, buffer, allocation = placed.allocation] {
        vmaDestroyBuffer(impl->allocator, buffer, allocation);
      },
      memory_info_of(impl_->allocator, placed.info.memoryType));
  owned.release();
  return result;
} catch (...) {
  return exception_status("Allocator::create_buffer",
                          Status::Code::InvalidArgument);
}

Result<Image> Allocator::create_image(const ImageDesc& desc) try {
  if (impl_ == nullptr) {
    return Status::invalid_argument("create_image: allocator is moved-from");
  }
  VKC_TRY(check_image_desc(desc, "create_image"));
  return make_image(desc, "create_image", nullptr, nullptr, nullptr, nullptr);
} catch (...) {
  return exception_status("Allocator::create_image",
                          Status::Code::InvalidArgument);
}

Status Allocator::check_image(const ImageDesc& desc, const char* caller) {
  return check_image_desc(desc, caller);
}

// What it throws, its callers catch: each names itself in the status.
Result<Image> Allocator::make_image(const ImageDesc& desc, const char* caller,
                                    const void* image_next, void* export_info,
                                    VkDeviceMemory* memory,
                                    VkDeviceSize* memory_size) {
  VKC_ASSIGN(const Sharing sharing,
             sharing_for(desc.queue_families, desc.queue_family_count,
                         impl_->queue_family_count, caller));

  // Zeroed, then every field set below -- samples included, which has no
  // zero enumerator.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.pNext = image_next;
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
  Resource resource;
  resource.image = image;
  VkMemoryDedicatedAllocateInfo dedicated{};
  if (export_info != nullptr) {
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = export_info;
    dedicated.image = image;
    resource.exported = &dedicated;
  }
  OwnedResource owned(impl_->view(*this), resource);
  // The host access is ignored for device-only memory.
  VKC_TRY(place(impl_->view(*this), owned, MemoryUsage::DeviceOnly,
                HostAccess::SequentialWrite, caller));
  const Placed& placed = owned.placed;
  VmaAllocation allocation = placed.allocation;

  VkImageView view = VK_NULL_HANDLE;
  if (desc.with_view) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = view_type_for(desc.type, desc.array_layers, desc.cube);
    view_info.format = desc.format;
    view_info.subresourceRange.aspectMask = view_aspect(desc.format);
    view_info.subresourceRange.levelCount = desc.mip_levels;
    view_info.subresourceRange.layerCount = desc.array_layers;
    const VkResult viewed =
        vkCreateImageView(impl_->device, &view_info, nullptr, &view);
    if (viewed != VK_SUCCESS) {
      return vk_error(viewed, "vkCreateImageView");
    }
    owned.view = view;
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
  info.memory = memory_info_of(impl_->allocator, placed.info.memoryType);
  // The view goes first: it references the image.
  Image result(info, [impl = impl_, view, image, allocation] {
    if (view != VK_NULL_HANDLE) {
      vkDestroyImageView(impl->device, view, nullptr);
    }
    vmaDestroyImage(impl->allocator, image, allocation);
  });
  if (export_info != nullptr) {
    *memory = placed.info.deviceMemory;
    *memory_size = placed.info.size;
  }
  owned.release();
  return result;
}

Result<Buffer> Allocator::bind_exported(VkBuffer buffer, VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        void* export_info,
                                        VkDeviceMemory* memory,
                                        VkDeviceSize* memory_size) try {
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = export_info;
  dedicated.buffer = buffer;
  Resource resource;
  resource.buffer = buffer;
  resource.exported = &dedicated;
  OwnedResource owned(impl_->view(*this), resource);
  // The host access is ignored for device-only memory.
  VKC_TRY(place(impl_->view(*this), owned, MemoryUsage::DeviceOnly,
                HostAccess::SequentialWrite, "create_exported_buffer"));
  const Placed& placed = owned.placed;
  Buffer result(
      buffer, size, usage, VK_SHARING_MODE_EXCLUSIVE, nullptr,
      [impl = impl_, buffer, allocation = placed.allocation] {
        vmaDestroyBuffer(impl->allocator, buffer, allocation);
      },
      memory_info_of(impl_->allocator, placed.info.memoryType));
  *memory = placed.info.deviceMemory;
  *memory_size = placed.info.size;
  owned.release();
  return result;
} catch (...) {
  return exception_status("Allocator::bind_exported",
                          Status::Code::InvalidArgument);
}

MemoryStats Allocator::memory_stats() const {
  MemoryStats stats;
  if (impl_ == nullptr) return stats;
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(impl_->allocator, &props);
  if (props == nullptr) return stats;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
  vmaGetHeapBudgets(impl_->allocator, budgets);
  return detail::memory_stats_of(budgets, props->memoryHeapCount);
}

VkDevice Allocator::device_handle() const noexcept {
  return impl_ != nullptr ? impl_->device : VK_NULL_HANDLE;
}

}  // namespace volumetric_kit::core
