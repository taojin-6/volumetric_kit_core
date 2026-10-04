// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Exercise the production allocation policy with real VMA suballocations and
// controlled budget figures. This executable owns a separate VMA instance;
// it does not link the core's allocator object or expose a public test hook.
#include "volumetric_kit/core/vulkan/vulkan.hpp"

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocation_budget.hpp"
#include "memory_types.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "vulkan_fixture.hpp"

namespace volumetric_kit::core {
namespace {

TEST(AllocationBudget, PreservesErrorsOtherThanDeviceMemoryExhaustion) {
  const VkPhysicalDeviceMemoryProperties memory{};
  const VkMemoryRequirements needs{};
  unsigned attempts = 0;
  unsigned budget_reads = 0;
  const VkResult result = detail::allocate_with_budget(
      memory, needs, 1, /*mapped=*/false, /*dedicated_required=*/false,
      [&](const VmaAllocationCreateInfo& info) {
        ++attempts;
        EXPECT_NE(info.flags & VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT, 0U);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
      },
      [&] {
        ++budget_reads;
        return MemoryStats{};
      });
  EXPECT_EQ(result, VK_ERROR_OUT_OF_HOST_MEMORY);
  EXPECT_EQ(attempts, 1U);
  EXPECT_EQ(budget_reads, 0U);
}

class AllocationBudgetTest : public test::VulkanTest {
 protected:
  void SetUp() override {
    VulkanTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    Result<Device> made =
        Device::create(instance(), physical(), requirements());
    ASSERT_TRUE(made.ok()) << made.status().message();
    device_.emplace(*std::move(made));
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo info{};
    info.instance = instance().handle();
    info.physicalDevice = physical().handle();
    info.device = device_->handle();
    info.vulkanApiVersion = VK_API_VERSION_1_1;
    info.pVulkanFunctions = &functions;
    ASSERT_EQ(vmaCreateAllocator(&info, &allocator_), VK_SUCCESS);
  }

  void TearDown() override {
    for (VmaAllocation allocation : allocations_) {
      vmaFreeMemory(allocator_, allocation);
    }
    if (allocator_ != nullptr) vmaDestroyAllocator(allocator_);
    device_.reset();
    VulkanTest::TearDown();
  }

  VkDevice device() const {
    if (!device_.has_value()) {
      ADD_FAILURE() << "test device is not initialized";
      return VK_NULL_HANDLE;
    }
    return device_->handle();
  }

  MemoryStats statistics() const {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator_, budgets);
    MemoryStats result;
    result.heap_count = physical().memory_properties().memoryHeapCount;
    for (std::uint32_t i = 0; i < result.heap_count; ++i) {
      result.heaps[i].usage_bytes = budgets[i].statistics.blockBytes;
      result.heaps[i].allocation_bytes = budgets[i].statistics.allocationBytes;
      result.heaps[i].heap_usage_bytes = budgets[i].usage;
      result.heaps[i].budget_bytes = budgets[i].budget;
    }
    return result;
  }

  void exhaust_budget() {
    budget_ = statistics();
    for (std::uint32_t i = 0; i < budget_->heap_count; ++i) {
      budget_->heaps[i].budget_bytes = budget_->heaps[i].heap_usage_bytes;
    }
  }

  template <typename Allocate>
  VkResult allocate(const VkMemoryRequirements& needs, bool dedicated,
                    Allocate&& operation, VmaAllocationInfo& out) {
    const auto& memory = physical().memory_properties();
    const std::uint32_t types = detail::placement_types(
        memory, MemoryUsage::DeviceOnly, HostAccess::SequentialWrite,
        needs.memoryTypeBits);
    VmaAllocation allocation = nullptr;
    const VkResult result = detail::allocate_with_budget(
        memory, needs, types, /*mapped=*/false, dedicated,
        [&](const VmaAllocationCreateInfo& info) {
          return operation(info, &allocation, &out);
        },
        [&] { return budget_.has_value() ? *budget_ : statistics(); });
    if (result == VK_SUCCESS) allocations_.push_back(allocation);
    return result;
  }

  VmaAllocator allocator() const { return allocator_; }

 private:
  std::optional<Device> device_;
  VmaAllocator allocator_ = nullptr;
  std::vector<VmaAllocation> allocations_;
  std::optional<MemoryStats> budget_;
};

TEST_F(AllocationBudgetTest, ReusesBufferMemoryAtTheHeapBudget) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = 256;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer raw = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateBuffer(device(), &info, nullptr, &raw), VK_SUCCESS);
  const UniqueHandle<VkBuffer, vkDestroyBuffer> first(device(), raw);
  ASSERT_EQ(vkCreateBuffer(device(), &info, nullptr, &raw), VK_SUCCESS);
  const UniqueHandle<VkBuffer, vkDestroyBuffer> second(device(), raw);

  VkMemoryDedicatedRequirements dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 needs{};
  needs.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  needs.pNext = &dedicated;
  VkBufferMemoryRequirementsInfo2 query{};
  query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
  query.buffer = first.get();
  vkGetBufferMemoryRequirements2(device(), &query, &needs);
  if (dedicated.requiresDedicatedAllocation == VK_TRUE) {
    GTEST_SKIP() << "This buffer requires dedicated memory";
  }

  VmaAllocationInfo before{};
  ASSERT_EQ(allocate(
                needs.memoryRequirements, false,
                [&](const VmaAllocationCreateInfo& allocation_info,
                    VmaAllocation* allocation, VmaAllocationInfo* out) {
                  return vmaAllocateMemoryForBuffer(allocator(), first.get(),
                                                    &allocation_info,
                                                    allocation, out);
                },
                before),
            VK_SUCCESS);
  exhaust_budget();
  const MemoryStats reserved = statistics();
  VmaAllocationInfo after{};
  ASSERT_EQ(allocate(
                needs.memoryRequirements, false,
                [&](const VmaAllocationCreateInfo& allocation_info,
                    VmaAllocation* allocation, VmaAllocationInfo* out) {
                  return vmaAllocateMemoryForBuffer(allocator(), second.get(),
                                                    &allocation_info,
                                                    allocation, out);
                },
                after),
            VK_SUCCESS);
  EXPECT_EQ(after.deviceMemory, before.deviceMemory);
  EXPECT_NE(after.offset, before.offset);
  const MemoryStats reused = statistics();
  for (std::uint32_t i = 0; i < reserved.heap_count; ++i) {
    EXPECT_EQ(reused.heaps[i].usage_bytes, reserved.heaps[i].usage_bytes);
  }
}

TEST_F(AllocationBudgetTest, ReusesImageMemoryAtTheHeapBudget) {
  const VkImageCreateInfo info{
      VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      nullptr,
      0,
      VK_IMAGE_TYPE_2D,
      VK_FORMAT_R8G8B8A8_UNORM,
      {16, 16, 1},
      1,
      1,
      VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      VK_SHARING_MODE_EXCLUSIVE,
      0,
      nullptr,
      VK_IMAGE_LAYOUT_UNDEFINED};
  VkImage raw = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImage(device(), &info, nullptr, &raw), VK_SUCCESS);
  const UniqueHandle<VkImage, vkDestroyImage> first(device(), raw);
  ASSERT_EQ(vkCreateImage(device(), &info, nullptr, &raw), VK_SUCCESS);
  const UniqueHandle<VkImage, vkDestroyImage> second(device(), raw);

  VkMemoryDedicatedRequirements dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 needs{};
  needs.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  needs.pNext = &dedicated;
  VkImageMemoryRequirementsInfo2 query{};
  query.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
  query.image = first.get();
  vkGetImageMemoryRequirements2(device(), &query, &needs);
  if (dedicated.requiresDedicatedAllocation == VK_TRUE) {
    GTEST_SKIP() << "This image requires dedicated memory";
  }

  VmaAllocationInfo before{};
  ASSERT_EQ(allocate(
                needs.memoryRequirements, false,
                [&](const VmaAllocationCreateInfo& allocation_info,
                    VmaAllocation* allocation, VmaAllocationInfo* out) {
                  return vmaAllocateMemoryForImage(allocator(), first.get(),
                                                   &allocation_info, allocation,
                                                   out);
                },
                before),
            VK_SUCCESS);
  exhaust_budget();
  const MemoryStats reserved = statistics();
  VmaAllocationInfo after{};
  ASSERT_EQ(allocate(
                needs.memoryRequirements, false,
                [&](const VmaAllocationCreateInfo& allocation_info,
                    VmaAllocation* allocation, VmaAllocationInfo* out) {
                  return vmaAllocateMemoryForImage(allocator(), second.get(),
                                                   &allocation_info, allocation,
                                                   out);
                },
                after),
            VK_SUCCESS);
  EXPECT_EQ(after.deviceMemory, before.deviceMemory);
  EXPECT_NE(after.offset, before.offset);
  const MemoryStats reused = statistics();
  for (std::uint32_t i = 0; i < reserved.heap_count; ++i) {
    EXPECT_EQ(reused.heaps[i].usage_bytes, reserved.heaps[i].usage_bytes);
  }
}

TEST_F(AllocationBudgetTest, RefusesNewDedicatedMemoryAtTheHeapBudget) {
  exhaust_budget();
  VkMemoryRequirements needs{};
  needs.size = 256;
  needs.alignment = 1;
  needs.memoryTypeBits = ~0U;
  VmaAllocationInfo out{};
  bool allocated = false;
  EXPECT_EQ(allocate(
                needs, true,
                [&](const VmaAllocationCreateInfo& info,
                    VmaAllocation* allocation, VmaAllocationInfo* result) {
                  allocated = true;
                  return vmaAllocateDedicatedMemory(
                      allocator(), &needs, &info, nullptr, allocation, result);
                },
                out),
            VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_FALSE(allocated);
}

TEST_F(AllocationBudgetTest, RequiredDedicatedMemorySkipsBlockReuse) {
  VkMemoryRequirements needs{};
  needs.size = 256;
  needs.alignment = 1;
  needs.memoryTypeBits = ~0U;
  VmaAllocationInfo out{};
  ASSERT_EQ(allocate(
                needs, true,
                [&](const VmaAllocationCreateInfo& info,
                    VmaAllocation* allocation, VmaAllocationInfo* result) {
                  EXPECT_EQ(
                      info.flags & VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT,
                      0U);
                  return vmaAllocateDedicatedMemory(
                      allocator(), &needs, &info, nullptr, allocation, result);
                },
                out),
            VK_SUCCESS);
  const MemoryStats stats = statistics();
  EXPECT_EQ(stats
                .heaps[physical()
                           .memory_properties()
                           .memoryTypes[out.memoryType]
                           .heapIndex]
                .usage_bytes,
            needs.size);
}

}  // namespace
}  // namespace volumetric_kit::core
