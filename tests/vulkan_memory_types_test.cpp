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
constexpr VkMemoryPropertyFlags kDeviceCoherent =
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
constexpr VkMemoryHeapFlags kLocalHeap = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
constexpr std::uint32_t kAny = ~0U;  // a resource any type suits

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

// The types a resource allowed `allowed` takes for `memory` and `access`.
std::uint32_t placed(const VkPhysicalDeviceMemoryProperties& props,
                     MemoryUsage memory, std::uint32_t allowed = kAny,
                     HostAccess access = HostAccess::SequentialWrite) {
  return detail::placement_types(props, memory, access, allowed);
}

// Separate heaps for private device memory, host memory and a mapped window.
VkPhysicalDeviceMemoryProperties discrete_layout() {
  return layout({kLocalHeap, 0, kLocalHeap}, {{1, 0},
                                              {0, kDL},
                                              {0, kDL},
                                              {1, kHV | kHC},
                                              {1, kHV | kHC | kCached},
                                              {2, kDL | kHV | kHC}});
}

// Duplicate private types remain eligible without admitting the mapped window.
TEST(MemoryTypes, DiscreteDuplicateTypesKeepDeviceOnlyOutOfTheBar) {
  const auto props = discrete_layout();
  EXPECT_EQ(detail::device_only_types(props), bits({1, 2}));
  EXPECT_EQ(detail::device_local_types(props), bits({1, 2, 5}));
  // Host-written device data is the BAR window; staging is system RAM.
  EXPECT_EQ(detail::device_mapped_types(props), bits({5}));
  EXPECT_EQ(detail::staging_types(props), bits({3, 4}));
  EXPECT_FALSE(detail::unified_memory(props));
}

// The same placements hold when cached host memory follows the mapped window.
TEST(MemoryTypes, DiscreteReorderedTypesKeepDeviceOnlyOutOfTheBar) {
  const auto props =
      layout({kLocalHeap, 0, kLocalHeap}, {{0, kDL},
                                           {1, kHV | kHC},
                                           {2, kDL | kHV | kHC},
                                           {1, kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::device_private_types(props), bits({0}));
  EXPECT_EQ(detail::device_mapped_types(props), bits({2}));
  EXPECT_EQ(detail::staging_types(props), bits({1, 3}));
  EXPECT_FALSE(detail::unified_memory(props));
}

// One unified heap with private, shared and memoryless
// storage. Device-only is the private storage, never memoryless.
TEST(MemoryTypes, UnifiedMemoryUsesPrivateStorageWhereAvailable) {
  const auto props =
      layout({kLocalHeap},
             {{0, kDL}, {0, kDL | kHV | kHC | kCached}, {0, kDL | kLazy}});
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::device_local_types(props), bits({0, 1}));
  EXPECT_EQ(detail::device_mapped_types(props), bits({1}));  // shared storage
  EXPECT_EQ(detail::staging_types(props), bits({1}));
  EXPECT_TRUE(detail::unified_memory(props));
}

// A single general type is both device-local and host-visible.
TEST(MemoryTypes, OneHostVisibleDeviceLocalTypeIsUnified) {
  const auto props = layout({kLocalHeap}, {{0, kDL | kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_private_types(props), 0U);
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::device_mapped_types(props), bits({0}));
  EXPECT_EQ(detail::staging_types(props), bits({0}));
  EXPECT_TRUE(detail::unified_memory(props));
}

// Unified layouts may expose private types or only host-visible ones.
// Memoryless types remain excluded in either case.
TEST(MemoryTypes, UnifiedLayoutsWithAndWithoutPrivateTypes) {
  const auto private_and_mapped =
      layout({kLocalHeap},
             {{0, kDL}, {0, kDL | kHV | kHC}, {0, kDL | kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_only_types(private_and_mapped), bits({0}));
  EXPECT_TRUE(detail::unified_memory(private_and_mapped));

  const auto mapped_and_memoryless = layout(
      {kLocalHeap},
      {{0, kDL | kHV | kHC}, {0, kDL | kHV | kHC | kCached}, {0, kDL | kLazy}});
  EXPECT_EQ(detail::device_only_types(mapped_and_memoryless), bits({0, 1}));
  EXPECT_TRUE(detail::unified_memory(mapped_and_memoryless));
}

// An integrated device whose driver reports a VRAM carve-out beside host memory
// is placed as its driver describes it, as a small discrete GPU: kernel data in
// the carve-out, which fails when full, and staging in host memory (GTT).
TEST(MemoryTypes, AnApuWithACarveOutIsNotUnified) {
  const auto props =
      layout({kLocalHeap, 0}, {{0, kDL}, {1, kHV | kHC}, {0, kDL | kHV | kHC}});
  EXPECT_FALSE(detail::unified_memory(props));
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::device_mapped_types(props), bits({2}));
  EXPECT_EQ(detail::staging_types(props), bits({1}));  // GTT, not the carve-out
  // A resource the carve-out's private type does not suit is refused, not
  // moved into the mapped part of the carve-out.
  EXPECT_EQ(placed(props, MemoryUsage::DeviceOnly, bits({1, 2})), 0U);
}

// A discrete GPU without a host-mappable device-local type has no
// device-mapped placement: the allocation is refused, never moved to host
// memory.
TEST(MemoryTypes, NoBarWindowMeansNoDeviceMappedMemory) {
  const auto props = layout(
      {kLocalHeap, 0}, {{0, kDL}, {1, kHV | kHC}, {1, kHV | kHC | kCached}});
  EXPECT_EQ(detail::device_mapped_types(props), 0U);
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(detail::staging_types(props), bits({1, 2}));
}

TEST(MemoryTypes, ProtectedAndLazyMemoryIsNeverChosen) {
  const auto props =
      layout({kLocalHeap},
             {{0, kDL | kProtected}, {0, kDL | kLazy}, {0, kDL | kHV | kHC}});
  EXPECT_EQ(detail::device_private_types(props), 0U);
  EXPECT_EQ(detail::device_only_types(props), bits({2}));
  EXPECT_EQ(detail::memory_types_with(props, kHV, 0), bits({2}));
}

// Device-coherent memory needs a feature this tier never enables, and
// VMA leaves it out: a device whose only coherent mapped VRAM is that kind has
// no device-mapped memory, and kernel data never lands there.
TEST(MemoryTypes, DeviceCoherentExtensionMemoryIsNeverChosen) {
  const auto props =
      layout({kLocalHeap, 0}, {{0, kDL},
                               {1, kHV | kHC},
                               {0, kDL | kHV | kHC | kDeviceCoherent},
                               {0, kDL | kDeviceCoherent}});
  EXPECT_EQ(detail::device_mapped_types(props), 0U);
  EXPECT_EQ(detail::device_local_types(props), bits({0}));
  EXPECT_EQ(detail::device_only_types(props), bits({0}));
  EXPECT_EQ(placed(props, MemoryUsage::DeviceMapped), 0U);
}

TEST(MemoryTypes, NoHeapsIsNotUnified) {
  const VkPhysicalDeviceMemoryProperties none{};
  EXPECT_FALSE(detail::unified_memory(none));
  EXPECT_EQ(detail::device_only_types(none), 0U);
}

// --- one resource's placement
// -------------------------------------------------

// A device-only resource takes the private types it allows; one that allows
// none on a discrete GPU is refused rather than placed in the BAR window.
TEST(MemoryTypes, DeviceOnlyNeverFallsBackToTheBarWindow) {
  const auto props = discrete_layout();
  EXPECT_EQ(placed(props, MemoryUsage::DeviceOnly), bits({1, 2}));
  EXPECT_EQ(placed(props, MemoryUsage::DeviceOnly, bits({2, 5})), bits({2}));
  EXPECT_EQ(placed(props, MemoryUsage::DeviceOnly, bits({5})), 0U);
  EXPECT_EQ(placed(props, MemoryUsage::DeviceOnly, bits({3, 4, 5})), 0U);
}

// On unified memory every device-local type is the one pool, so a resource
// no private type suits -- such as a linear image -- takes the rest of it.
TEST(MemoryTypes, DeviceOnlyTakesTheOnePoolOnUnifiedMemory) {
  const auto unified =
      layout({kLocalHeap},
             {{0, kDL}, {0, kDL | kHV | kHC | kCached}, {0, kDL | kLazy}});
  EXPECT_EQ(placed(unified, MemoryUsage::DeviceOnly), bits({0}));
  EXPECT_EQ(placed(unified, MemoryUsage::DeviceOnly, bits({1, 2})), bits({1}));
  EXPECT_EQ(placed(unified, MemoryUsage::DeviceOnly, bits({2})), 0U);  // lazy
}

// SequentialWrite prefers write-combined types and Random cached ones; a
// device-mapped buffer the host reads requires cached memory, so an
// uncached device-local type is refused.
TEST(MemoryTypes, HostAccessNarrowsTheMappedTypes) {
  const auto props = discrete_layout();
  EXPECT_EQ(placed(props, MemoryUsage::Staging), bits({3}));
  EXPECT_EQ(placed(props, MemoryUsage::Staging, kAny, HostAccess::Random),
            bits({4}));
  EXPECT_EQ(placed(props, MemoryUsage::DeviceMapped), bits({5}));
  EXPECT_EQ(placed(props, MemoryUsage::DeviceMapped, kAny, HostAccess::Random),
            0U);

  const auto private_and_mapped =
      layout({kLocalHeap},
             {{0, kDL}, {0, kDL | kHV | kHC}, {0, kDL | kHV | kHC | kCached}});
  EXPECT_EQ(placed(private_and_mapped, MemoryUsage::DeviceMapped), bits({1}));
  EXPECT_EQ(placed(private_and_mapped, MemoryUsage::DeviceMapped, kAny,
                   HostAccess::Random),
            bits({2}));

  // With one uncached coherent type, a staging readback still
  // gets it; a device-mapped one the host reads does not.
  const auto uncached_only = layout({kLocalHeap}, {{0, kDL | kHV | kHC}});
  EXPECT_EQ(
      placed(uncached_only, MemoryUsage::Staging, kAny, HostAccess::Random),
      bits({0}));
  EXPECT_EQ(placed(uncached_only, MemoryUsage::DeviceMapped, kAny,
                   HostAccess::Random),
            0U);
  // Only cached memory: a sequential writer takes it.
  const auto cached_only =
      layout({kLocalHeap}, {{0, kDL | kHV | kHC | kCached}});
  EXPECT_EQ(placed(cached_only, MemoryUsage::DeviceMapped), bits({0}));
}

// Device-mapped memory is cached only where a device-local, host-visible,
// host-coherent type is host-cached too -- exactly where a DeviceMapped buffer
// may take HostAccess::Random, and so where a HostRing binds in place.
TEST(MemoryTypes, DeviceMappedMemoryIsCachedOnlyWhereTheHostReadsIt) {
  const auto expect = [](const VkPhysicalDeviceMemoryProperties& props,
                         bool cached) {
    EXPECT_EQ(detail::device_mapped_cached(props), cached);
    EXPECT_EQ(
        placed(props, MemoryUsage::DeviceMapped, kAny, HostAccess::Random) != 0,
        cached);
  };
  {
    SCOPED_TRACE("a unified cached pool");
    expect(layout({kLocalHeap}, {{0, kDL}, {0, kDL | kHV | kHC | kCached}}),
           true);
  }
  {
    SCOPED_TRACE("one type with every flag (a CPU implementation)");
    expect(layout({kLocalHeap}, {{0, kDL | kHV | kHC | kCached}}), true);
  }
  {
    SCOPED_TRACE("a discrete GPU with a small BAR heap");
    expect(discrete_layout(), false);
  }
  {
    SCOPED_TRACE("Resizable BAR: all of VRAM mapped, write-combined");
    expect(layout({kLocalHeap, 0}, {{0, kDL},
                                    {1, kHV | kHC},
                                    {1, kHV | kHC | kCached},
                                    {0, kDL | kHV | kHC}}),
           false);
  }
  {
    SCOPED_TRACE("a unified uncached coherent pool");
    expect(layout({kLocalHeap}, {{0, kDL}, {0, kDL | kHV | kHC}}), false);
  }
  {
    SCOPED_TRACE("a unified cached pool that is not coherent");
    expect(layout({kLocalHeap}, {{0, kDL}, {0, kDL | kHV | kCached}}), false);
  }
  {
    SCOPED_TRACE("no mapped type");
    expect(layout({kLocalHeap, 0},
                  {{0, kDL}, {1, kHV | kHC}, {1, kHV | kHC | kCached}}),
           false);
  }
}

// A heap's figures: its usage and budget, and this allocator's blocks in it.
HeapStats heap(std::uint64_t usage, std::uint64_t budget,
               std::uint64_t reserved = 0) {
  HeapStats figures;
  figures.usage_bytes = usage;
  figures.budget_bytes = budget;
  figures.reserved_bytes = reserved;
  return figures;
}

// New device memory is admitted against the heap's usage, which can include
// other allocators' memory, never against this allocator's blocks alone.
TEST(MemoryTypes, NewMemoryIsAdmittedAgainstTheHeapUsage) {
  const auto props = discrete_layout();  // types 1, 2 on heap 0; 3, 4 on heap 1
  MemoryStats heaps;
  heaps.heap_count = 3;
  heaps.heaps[0] = heap(900, 1000, 50);  // 100 bytes left
  heaps.heaps[1] = heap(0, 1000);
  heaps.heaps[2] = heap(0, 1000);
  EXPECT_EQ(detail::types_within_budget(props, bits({1, 2}), 100, heaps),
            bits({1, 2}));
  EXPECT_EQ(detail::types_within_budget(props, bits({1, 2}), 101, heaps), 0U);
  EXPECT_EQ(detail::types_within_budget(props, bits({2, 3}), 101, heaps),
            bits({3}));
  // Past the budget already, or asking for more than it: never wraps.
  heaps.heaps[0] = heap(1200, 1000, 50);
  EXPECT_EQ(detail::types_within_budget(props, bits({1}), 1, heaps), 0U);
  EXPECT_EQ(
      detail::types_within_budget(props, bits({3}), ~VkDeviceSize{0}, heaps),
      0U);
  // A heap the figures do not cover (a moved-from allocator's) has no room.
  heaps.heap_count = 0;
  EXPECT_EQ(detail::types_within_budget(props, bits({1, 3}), 1, heaps), 0U);
}

// The search for a resource bound outside the allocator places device-local
// memory as DeviceOnly does, unless the caller asks to map it.
TEST(MemoryTypes, DeviceLocalSearchStaysOutOfTheBarWindow) {
  const auto props = discrete_layout();
  EXPECT_EQ(detail::first_memory_type(props, kAny, kDL, 0), 1U);
  EXPECT_EQ(detail::first_memory_type(props, bits({2, 5}), kDL, 0), 2U);
  EXPECT_FALSE(detail::first_memory_type(props, bits({5}), kDL, 0));
  EXPECT_EQ(detail::first_memory_type(props, bits({5}), kDL | kHV, 0), 5U);
  EXPECT_EQ(detail::first_memory_type(props, kAny, kHV, 0), 3U);

  // On unified memory a resource no private type suits takes the pool.
  const auto unified = layout({kLocalHeap}, {{0, kDL}, {0, kDL | kHV | kHC}});
  EXPECT_EQ(detail::first_memory_type(unified, bits({1}), kDL, 0), 1U);
  EXPECT_FALSE(detail::first_memory_type(unified, bits({1}), kDL, kHV));
}

// The resource's own requirements cut every placement.
TEST(MemoryTypes, APlacementIsCutToWhatTheResourceAllows) {
  const auto props = discrete_layout();
  EXPECT_EQ(placed(props, MemoryUsage::Staging, bits({4})), bits({4}));
  EXPECT_EQ(placed(props, MemoryUsage::Staging, bits({1, 5})), 0U);
  EXPECT_EQ(placed(props, MemoryUsage::DeviceMapped, bits({1, 2})), 0U);
}

// --- images, and transient attachments ---------------------------------------
// On the layouts drivers report, as measured: MoltenVK on Apple GPUs (one
// unified heap with private, shared and memoryless types, the same on a laptop
// and a phone), a discrete GPU with five types and no lazily allocated memory,
// and lavapipe's single type.

constexpr VkImageUsageFlags kColor = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
constexpr VkImageUsageFlags kDepth =
    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
constexpr VkImageUsageFlags kTransient =
    VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

VkPhysicalDeviceMemoryProperties moltenvk_layout() {
  return layout({kLocalHeap},
                {{0, kDL}, {0, kDL | kHV | kHC | kCached}, {0, kDL | kLazy}});
}

VkPhysicalDeviceMemoryProperties five_type_discrete_layout() {
  return layout({kLocalHeap, 0}, {{1, 0},
                                  {0, kDL},
                                  {1, kHV | kHC},
                                  {1, kHV | kHC | kCached},
                                  {0, kDL | kHV | kHC}});
}

VkPhysicalDeviceMemoryProperties lavapipe_layout() {
  return layout({kLocalHeap}, {{0, kDL | kHV | kHC | kCached}});
}

// A transient attachment takes the lazily allocated types it allows -- on
// MoltenVK, Metal's memoryless storage -- and nothing else does, even where
// a resource's requirements allowed them.
TEST(MemoryTypes, ATransientAttachmentTakesLazilyAllocatedMemory) {
  const auto props = moltenvk_layout();
  EXPECT_EQ(detail::lazily_allocated_types(props), bits({2}));
  EXPECT_EQ(detail::image_types(props, kColor | kTransient, bits({0, 2})),
            bits({2}));
  EXPECT_EQ(detail::image_types(props, kDepth | kTransient, kAny), bits({2}));
  // Not transient: device-only, the private type.
  EXPECT_EQ(detail::image_types(props, kColor, kAny), bits({0}));
  EXPECT_EQ(detail::image_types(props, kColor, bits({2})), 0U);
  // Transient, but its requirements leave the lazy type out: device-only.
  EXPECT_EQ(detail::image_types(props, kColor | kTransient, bits({0, 1})),
            bits({0}));
  // No other placement, nor the search for memory bound outside the
  // allocator, ever takes it.
  for (const MemoryUsage memory :
       {MemoryUsage::DeviceOnly, MemoryUsage::DeviceMapped,
        MemoryUsage::Staging}) {
    EXPECT_EQ(placed(props, memory) & bits({2}), 0U);
    EXPECT_EQ(placed(props, memory, kAny, HostAccess::Random) & bits({2}), 0U);
  }
  EXPECT_FALSE(detail::first_memory_type(props, bits({2}), kDL, 0));
}

// Without lazily allocated memory a transient attachment is device-only,
// as any image: a discrete GPU's VRAM outside the BAR window, lavapipe's one
// type.
TEST(MemoryTypes, ATransientAttachmentIsDeviceOnlyWithoutLazyMemory) {
  const auto discrete = five_type_discrete_layout();
  EXPECT_EQ(detail::lazily_allocated_types(discrete), 0U);
  EXPECT_EQ(detail::image_types(discrete, kColor | kTransient, kAny),
            bits({1}));
  EXPECT_EQ(detail::image_types(discrete, kDepth | kTransient, kAny),
            placed(discrete, MemoryUsage::DeviceOnly));
  EXPECT_EQ(detail::image_types(discrete, kColor | kTransient, bits({4})), 0U)
      << "never the BAR window";

  const auto cpu = lavapipe_layout();
  EXPECT_EQ(detail::lazily_allocated_types(cpu), 0U);
  EXPECT_EQ(detail::image_types(cpu, kColor | kTransient, kAny), bits({0}));
}

// A lazily allocated type that is also protected is not one a transient
// attachment may take.
TEST(MemoryTypes, OnlyPlainLazilyAllocatedTypesServeTransientAttachments) {
  const auto props =
      layout({kLocalHeap}, {{0, kDL}, {0, kDL | kLazy | kProtected}});
  EXPECT_EQ(detail::lazily_allocated_types(props), 0U);
  EXPECT_EQ(detail::image_types(props, kColor | kTransient, kAny), bits({0}));
}

}  // namespace
}  // namespace volumetric_kit::core
