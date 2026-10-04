// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// UniqueHandle against fake deleters, so these need no device: one a
// function, as the link-time loader declares vkDestroy*, and one a variable
// holding a pointer, as volk does.

#include "volumetric_kit/core/vulkan/unique_handle.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

// Distinct non-null handles of any kind, from the addresses of these bytes:
// a handle is a pointer on 64-bit targets and a uint64_t on 32-bit ones.
char handle_storage[3];

template <class Handle>
Handle fake_handle(int index) {
  char* address = &handle_storage[index];
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(address);
  } else {
    return static_cast<Handle>(reinterpret_cast<std::uintptr_t>(address));
  }
}

VkDevice fake_device() { return fake_handle<VkDevice>(0); }
VkFence fake_fence(int index) { return fake_handle<VkFence>(index); }

int destroyed = 0;
VkFence last_destroyed = VK_NULL_HANDLE;

void VKAPI_CALL fake_destroy_fence(VkDevice /*device*/, VkFence fence,
                                   const VkAllocationCallbacks* /*allocator*/) {
  ++destroyed;
  last_destroyed = fence;
}

// What volk declares in place of the prototype: the loaded entry point, in a
// variable read at each call.
PFN_vkDestroyFence loaded_destroy_fence = fake_destroy_fence;

using LinkedFence = UniqueHandle<VkFence, fake_destroy_fence>;
using LoadedFence = UniqueHandle<VkFence, loaded_destroy_fence>;

class UniqueHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    destroyed = 0;
    last_destroyed = VK_NULL_HANDLE;
  }
};

TEST_F(UniqueHandleTest, DestroysOnceThroughALinkedFunction) {
  VkDevice device = fake_device();
  VkFence fence = fake_fence(1);
  {
    const LinkedFence owner(device, fence);
    EXPECT_TRUE(owner.valid());
    EXPECT_EQ(owner.get(), fence);
    EXPECT_EQ(owner.device(), device);
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(last_destroyed, fence);
}

TEST_F(UniqueHandleTest, DestroysOnceThroughALoadedEntryPoint) {
  VkFence fence = fake_fence(1);
  {
    const LoadedFence owner(fake_device(), fence);
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(last_destroyed, fence);
}

TEST_F(UniqueHandleTest, MovesTransferOwnership) {
  VkFence first = fake_fence(1);
  VkFence second = fake_fence(2);
  {
    LoadedFence a(fake_device(), first);
    LoadedFence b = std::move(a);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(b.get(), first);
    EXPECT_EQ(destroyed, 0);

    LoadedFence c(fake_device(), second);
    c = std::move(b);  // frees `second`, takes `first`
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(last_destroyed, second);
    EXPECT_EQ(c.get(), first);
  }
  EXPECT_EQ(destroyed, 2);
  EXPECT_EQ(last_destroyed, first);
}

TEST_F(UniqueHandleTest, AnEmptyOwnerDestroysNothing) {
  {
    const LinkedFence empty;
  }
  EXPECT_EQ(destroyed, 0);
}

}  // namespace
}  // namespace volumetric_kit::core
