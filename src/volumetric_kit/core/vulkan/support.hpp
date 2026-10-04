// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: the pieces of the requirements check that
// check_device_support (selection, create) and Device::adopt share, and the
// device-creation pieces Device::create and SharedDevice share. Not
// installed.

#include <cstdint>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::detail {

// VK_KHR_portability_subset's name macro lives in vulkan_beta.h, behind
// VK_ENABLE_BETA_EXTENSIONS; the string is stable.
inline constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";

// "1.3" for a packed API version.
std::string version_text(std::uint32_t version);

// `version` with its patch zeroed, so versions compare as major.minor.
inline std::uint32_t without_patch(std::uint32_t version) {
  return VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(version),
                             VK_API_VERSION_MAJOR(version),
                             VK_API_VERSION_MINOR(version), 0);
}

// reqs.extensions, plus VK_KHR_swapchain when it needs presentation, without
// duplicates and in that order.
std::vector<std::string> required_extensions(const DeviceRequirements& reqs);

// VkPhysicalDeviceFeatures is a contiguous block of VkBool32, so these walk
// it as an array, and a new core feature needs no edit here.

// Whether every VK_TRUE bit of `wanted` is VK_TRUE in `have`.
bool features_subset(const VkPhysicalDeviceFeatures& wanted,
                     const VkPhysicalDeviceFeatures& have);

// Every feature VK_TRUE in `a` or `b`.
VkPhysicalDeviceFeatures features_union(const VkPhysicalDeviceFeatures& a,
                                        const VkPhysicalDeviceFeatures& b);

// The device-level half of the check: the usable API version, the required
// extensions (`required_extensions(reqs)`, built once by the caller, which
// needs them too), the core features and the feature flags. No queue
// families.
Status check_physical_support(const PhysicalDeviceInfo& caps,
                              const DeviceRequirements& reqs,
                              const std::vector<std::string>& required);

// The extensions a device made for `reqs` enables: the required ones, then
// the portability subset (which the spec requires wherever it is exposed),
// VK_EXT_memory_budget, and `reqs.optional_extensions`, each where `caps`
// offers it.
std::vector<std::string> enabled_extensions(const PhysicalDeviceInfo& caps,
                                            const DeviceRequirements& reqs);

// The VkDeviceCreateInfo feature chain Device::create and SharedDevice build:
// the requirements' core features, timeline semaphores, scalar block layout and
// dynamic rendering, then the caller's chain. Built in place -- its nodes point
// at one another -- so it is never copied or moved once built.
struct FeatureChain {
  VkPhysicalDeviceFeatures2 features2{};
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
  VkPhysicalDeviceScalarBlockLayoutFeatures scalar{};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic{};

  FeatureChain() = default;
  FeatureChain(const FeatureChain&) = delete;
  FeatureChain& operator=(const FeatureChain&) = delete;

  void build(const DeviceRequirements& reqs) {
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features = reqs.features;
    timeline.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;
    scalar.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
    scalar.scalarBlockLayout = VK_TRUE;
    dynamic.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic.dynamicRendering = VK_TRUE;

    // A chain may not hold both a version aggregate and a struct it subsumes
    // (VUID-VkDeviceCreateInfo-pNext-02830). So where the caller's chain
    // already carries the aggregate or the standalone struct for a feature,
    // the bit is raised in the caller's struct, and this chain links its own
    // only otherwise.
    VkPhysicalDeviceVulkan12Features* v12 = nullptr;
    VkPhysicalDeviceVulkan13Features* v13 = nullptr;
    VkPhysicalDeviceTimelineSemaphoreFeatures* their_timeline = nullptr;
    VkPhysicalDeviceScalarBlockLayoutFeatures* their_scalar = nullptr;
    VkPhysicalDeviceDynamicRenderingFeatures* their_dynamic = nullptr;
    for (auto* node = static_cast<VkBaseOutStructure*>(reqs.feature_chain);
         node != nullptr; node = node->pNext) {
      switch (node->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
          v12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
          v13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
          their_timeline =
              reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(
                  node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
          their_scalar =
              reinterpret_cast<VkPhysicalDeviceScalarBlockLayoutFeatures*>(
                  node);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
          their_dynamic =
              reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(node);
          break;
        default:
          break;
      }
    }

    auto* tail = reinterpret_cast<VkBaseOutStructure*>(&features2);
    auto link = [&tail](void* feature) {
      auto* node = static_cast<VkBaseOutStructure*>(feature);
      node->pNext = nullptr;
      tail->pNext = node;
      tail = node;
    };
    if (reqs.timeline_semaphore) {
      if (v12 != nullptr) {
        v12->timelineSemaphore = VK_TRUE;
      } else if (their_timeline != nullptr) {
        their_timeline->timelineSemaphore = VK_TRUE;
      } else {
        link(&timeline);
      }
    }
    if (reqs.scalar_block_layout) {
      if (v12 != nullptr) {
        v12->scalarBlockLayout = VK_TRUE;
      } else if (their_scalar != nullptr) {
        their_scalar->scalarBlockLayout = VK_TRUE;
      } else {
        link(&scalar);
      }
    }
    if (reqs.dynamic_rendering) {
      if (v13 != nullptr) {
        v13->dynamicRendering = VK_TRUE;
      } else if (their_dynamic != nullptr) {
        their_dynamic->dynamicRendering = VK_TRUE;
      } else {
        link(&dynamic);
      }
    }
    tail->pNext = static_cast<VkBaseOutStructure*>(reqs.feature_chain);
  }
};

}  // namespace volumetric_kit::core::detail
