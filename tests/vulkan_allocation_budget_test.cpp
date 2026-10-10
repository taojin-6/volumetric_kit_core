// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Exercise the production allocation policy with real VMA suballocations and
// controlled budget figures, through a VMA allocator of the test's own rather
// than a public test hook.
//
// One VMA implementation only: a static core's comes from its archive with
// the allocator's object, and a shared core hides its own, so only then does
// this executable instantiate VMA itself (VKC_TEST_PRIVATE_VMA, set by
// tests/CMakeLists.txt). Two copies in one static link would fail on
// duplicate symbols as soon as the test touched the Allocator.
#include "volumetric_kit/core/vulkan/vulkan.hpp"

#ifdef VKC_TEST_PRIVATE_VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#endif
#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocation_budget.hpp"
#include "memory_types.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"

namespace volumetric_kit::core {
namespace {

// --- the policy alone, against scripted VMA results
// ---------------------------

// Two device-local types on one 1000-byte-budget heap, which `usage` bytes
// already fill.
struct Scripted {
  VkPhysicalDeviceMemoryProperties memory{};
  MemoryStats stats;
  VkMemoryRequirements needs{};

  explicit Scripted(std::uint64_t usage) {
    memory.memoryHeapCount = 1;
    memory.memoryHeaps[0] = {1U << 20, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    memory.memoryTypeCount = 2;
    memory.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    memory.memoryTypes[1] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    stats.heap_count = 1;
    stats.heaps[0].usage_bytes = usage;
    stats.heaps[0].budget_bytes = 1000;
    needs.size = 100;
    needs.memoryTypeBits = 0x3;
  }

  // Runs the policy over both types, recording each VMA call's parameters
  // and answering them from `results` in turn.
  detail::BudgetedAllocation run(std::vector<VkResult> results,
                                 std::vector<VmaAllocationCreateInfo>& calls,
                                 bool dedicated_required = false) const {
    return detail::allocate_with_budget(
        memory, needs, 0x3, /*mapped=*/false, dedicated_required,
        [&](const VmaAllocationCreateInfo& info) {
          calls.push_back(info);
          return calls.size() <= results.size() ? results[calls.size() - 1]
                                                : VK_ERROR_UNKNOWN;
        },
        [&] { return stats; });
  }
};

bool never_allocates(const VmaAllocationCreateInfo& info) {
  return (info.flags & VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT) != 0;
}

// With budget room VMA allocates as it would without one, free to make a new
// block of the first type or memory of the resource's own.
TEST(AllocationBudget, WithRoomVmaAllocatesUnrestricted) {
  std::vector<VmaAllocationCreateInfo> calls;
  const detail::BudgetedAllocation made = Scripted(0).run({VK_SUCCESS}, calls);
  EXPECT_EQ(made.result, VK_SUCCESS);
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_FALSE(never_allocates(calls[0]));
  EXPECT_EQ(calls[0].memoryTypeBits, 0x3U);
  EXPECT_NE(calls[0].flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT, 0U);
}

// An error other than exhaustion is returned as it is, without a retry.
TEST(AllocationBudget, PreservesErrorsOtherThanDeviceMemoryExhaustion) {
  std::vector<VmaAllocationCreateInfo> calls;
  const detail::BudgetedAllocation made =
      Scripted(0).run({VK_ERROR_OUT_OF_HOST_MEMORY}, calls);
  EXPECT_EQ(made.result, VK_ERROR_OUT_OF_HOST_MEMORY);
  EXPECT_FALSE(made.over_budget);
  EXPECT_EQ(calls.size(), 1U);
}

// At the budget only existing blocks are tried, in every candidate type.
TEST(AllocationBudget, AtTheBudgetOnlyBlocksAreReused) {
  std::vector<VmaAllocationCreateInfo> calls;
  const detail::BudgetedAllocation made =
      Scripted(950).run({VK_SUCCESS}, calls);
  EXPECT_EQ(made.result, VK_SUCCESS);
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_TRUE(never_allocates(calls[0]));
  EXPECT_EQ(calls[0].memoryTypeBits, 0x3U);
}

// A refusal for want of budget says so; one by VMA or the driver does not,
// though no block had room afterwards either.
TEST(AllocationBudget, TellsABudgetRefusalFromAnAllocationFailure) {
  std::vector<VmaAllocationCreateInfo> calls;
  const detail::BudgetedAllocation refused =
      Scripted(950).run({VK_ERROR_OUT_OF_DEVICE_MEMORY}, calls);
  EXPECT_EQ(refused.result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_TRUE(refused.over_budget);

  calls.clear();
  const detail::BudgetedAllocation failed = Scripted(0).run(
      {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}, calls);
  EXPECT_EQ(failed.result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_FALSE(failed.over_budget);
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_FALSE(never_allocates(calls[0]));
  EXPECT_TRUE(never_allocates(calls[1]));
}

// Memory a resource requires to itself is never taken from a block, so at
// the budget it is refused without a VMA call.
TEST(AllocationBudget, RequiredDedicatedMemoryIsNeverReused) {
  std::vector<VmaAllocationCreateInfo> calls;
  const detail::BudgetedAllocation refused =
      Scripted(950).run({}, calls, /*dedicated_required=*/true);
  EXPECT_EQ(refused.result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_TRUE(refused.over_budget);
  EXPECT_TRUE(calls.empty());
}

// --- the policy against real VMA allocations ---------------------------------

class AllocationBudgetTest : public test::VulkanTest {
 protected:
  void SetUp() override {
    VulkanTest::SetUp();
    if (base_setup_incomplete()) return;
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

  // The figures Allocator::memory_stats reports, by the same mapping.
  MemoryStats statistics() const {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator_, budgets);
    return detail::memory_stats_of(
        budgets, physical().memory_properties().memoryHeapCount);
  }

  void exhaust_budget() {
    budget_ = statistics();
    for (std::uint32_t i = 0; i < budget_->heap_count; ++i) {
      budget_->heaps[i].budget_bytes = budget_->heaps[i].usage_bytes;
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
    const VkResult result =
        detail::allocate_with_budget(
            memory, needs, types, /*mapped=*/false, dedicated,
            [&](const VmaAllocationCreateInfo& info) {
              return operation(info, &allocation, &out);
            },
            [&] { return budget_.has_value() ? *budget_ : statistics(); })
            .result;
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
    EXPECT_EQ(reused.heaps[i].reserved_bytes, reserved.heaps[i].reserved_bytes);
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
    EXPECT_EQ(reused.heaps[i].reserved_bytes, reserved.heaps[i].reserved_bytes);
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
                .reserved_bytes,
            needs.size);
}

}  // namespace
}  // namespace volumetric_kit::core
