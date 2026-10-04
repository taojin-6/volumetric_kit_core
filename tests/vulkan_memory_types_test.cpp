// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Where each MemoryUsage may land, from the memory layouts real drivers
// report -- discrete GPUs among them, which no CI runner has. Needs no device.

#include <cstdint>
#include <initializer_list>
#include <utility>

#include <gtest/gtest.h>

#include "memory_types.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

constexpr VkMemoryPropertyFlags kDL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr VkMemoryPropertyFlags kHV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr VkMemoryPropertyFlags kHC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags kCached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
constexpr VkMemoryPropertyFlags kLazy = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
constexpr VkMemoryPropertyFlags kProtected = VK_MEMORY_PROPERTY_PROTECTED_BIT;
constexpr VkMemoryHeapFlags kLocalHeap = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

// Heaps by flags; types as {heap, flags}.
VkPhysicalDeviceMemoryProperties layout(
    std::initializer_list<VkMemoryHeapFlags> heaps,
    std::initializer_list<std::pair<std::uint32_t, VkMemoryPropertyFlags>>
        types) {
  VkPhysicalDeviceMemoryProperties props{};
  for (const VkMemoryHeapFlags flags : heaps) {
    props.memoryHeaps[props.memoryHeapCount++].flags = flags;
  }
  for (const auto& [heap, flags] : types) {
    props.memoryTypes[props.memoryTypeCount++] = {flags, heap};
  }
  return props;
}

std::uint32_t bits(std::initializer_list<std::uint32_t> types) {
  std::uint32_t mask = 0;
  for (const std::uint32_t t : types) mask |= 1U << t;
  return mask;
}

// An NVIDIA discrete GPU: VRAM, system RAM, and the 256 MiB BAR window.
TEST(MemoryTypes, DiscreteNvidiaKeepsDeviceOnlyOutOfTheBar) {
  const auto props =
      layout({kLocalHeap, 0, kLocalHeap}, {{1, 0},
                                           {0, kDL},
                                           {0, kDL},
                                           {1, kHV | kHC},
                                           {1, kHV | kHC | kCached},
                                           {2, kDL | kHV | kHC}});
  EXPECT_EQ(detail::device_only_types(props), bits({1, 2}));
  EXPECT_EQ(detail::device_local_types(props), bits({1, 2, 5}));
  EXPECT_FALSE(
      detail::unified_memory(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
}

// An AMD discrete GPU (RADV): VRAM, system RAM, the BAR window.
TEST(MemoryTypes, DiscreteAmdKeepsDeviceOnlyOutOfTheBar) {
  const auto props =
      layout({kLocalHeap, 0, kLocalHeap}, {{0, kDL},
                                           {1, kHV | kHC},
                                           {2, kDL | kHV | kHC},
                                           {1, kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::device_private_types(props), bits({0}));
  EXPECT_FALSE(
      detail::unified_memory(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
}

// Apple silicon (MoltenVK): one heap; private, shared and memoryless
// storage. Device-only is the private storage, never memoryless.
TEST(MemoryTypes, AppleSiliconUsesPrivateStorage) {
  const auto props =
      layout({kLocalHeap},
             {{0, kDL}, {0, kDL | kHV | kHC | kCached}, {0, kDL | kLazy}});
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::device_local_types(props), bits({0, 1}));
  EXPECT_TRUE(
      detail::unified_memory(props, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU));
}

// lavapipe: one type, device-local and host-visible -- the device's one pool.
TEST(MemoryTypes, LavapipeHasOnePool) {
  const auto props = layout({kLocalHeap}, {{0, kDL | kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_private_types(props), 0U);
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_TRUE(detail::unified_memory(props, VK_PHYSICAL_DEVICE_TYPE_CPU));
}

// An Intel integrated GPU (ANV) has a device-only type; a Mali GPU does not,
// and its memoryless type stays out.
TEST(MemoryTypes, IntegratedAndMobileGpus) {
  const auto intel =
      layout({kLocalHeap},
             {{0, kDL}, {0, kDL | kHV | kHC}, {0, kDL | kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_only_types(intel), bits({0}));
  EXPECT_TRUE(
      detail::unified_memory(intel, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU));

  const auto mali = layout(
      {kLocalHeap},
      {{0, kDL | kHV | kHC}, {0, kDL | kHV | kHC | kCached}, {0, kDL | kLazy}});
  EXPECT_EQ(detail::device_only_types(mali), bits({0, 1}));
  EXPECT_TRUE(
      detail::unified_memory(mali, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU));
}

// An AMD APU reports a VRAM carve-out beside host memory, but shares one
// DRAM: unified by its type. The same heaps on a discrete GPU are not.
TEST(MemoryTypes, AnApuIsUnifiedByItsType) {
  const auto props =
      layout({kLocalHeap, 0}, {{0, kDL}, {1, kHV | kHC}, {0, kDL | kHV | kHC}});
  EXPECT_TRUE(
      detail::unified_memory(props, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU));
  EXPECT_FALSE(
      detail::unified_memory(props, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
}

TEST(MemoryTypes, ProtectedAndLazyMemoryIsNeverChosen) {
  const auto props =
      layout({kLocalHeap},
             {{0, kDL | kProtected}, {0, kDL | kLazy}, {0, kDL | kHV | kHC}});
  EXPECT_EQ(detail::device_private_types(props), 0U);
  EXPECT_EQ(detail::device_only_types(props), bits({2}));
  EXPECT_EQ(detail::memory_types_with(props, kHV, 0), bits({2}));
}

TEST(MemoryTypes, NoHeapsIsNotUnified) {
  const VkPhysicalDeviceMemoryProperties none{};
  EXPECT_FALSE(detail::unified_memory(none, VK_PHYSICAL_DEVICE_TYPE_OTHER));
  EXPECT_EQ(detail::device_only_types(none), 0U);
}

}  // namespace
}  // namespace volumetric_kit::core
