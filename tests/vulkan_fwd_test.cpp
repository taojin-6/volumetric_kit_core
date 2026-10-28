// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// fwd.hpp against the full headers, checked as this file compiles. Each name
// is taken where only fwd.hpp has been seen, so a class it misses, or declares
// in another namespace, fails here; the full headers must then complete that
// same class, so one they move or rename fails below. This target also makes a
// class-key mismatch (`struct` for `class`) an error (tests/CMakeLists.txt).

#include "volumetric_kit/core/vulkan/fwd.hpp"

#ifdef VK_HEADER_VERSION
#error "fwd.hpp must not include Vulkan"
#endif

namespace fwd {
using Allocator = volumetric_kit::core::Allocator;
using Buffer = volumetric_kit::core::Buffer;
using CommandBatch = volumetric_kit::core::CommandBatch;
using Device = volumetric_kit::core::Device;
using GpuStageScope = volumetric_kit::core::GpuStageScope;
using Image = volumetric_kit::core::Image;
using StorageInput = volumetric_kit::core::StorageInput;
}  // namespace fwd

#include <gtest/gtest.h>

#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"

namespace {

// alignof an incomplete class does not compile.
template <class T>
constexpr bool kComplete = alignof(T) > 0;

TEST(VulkanFwd, DeclaresTheClassesTheFullHeadersDefine) {
  static_assert(kComplete<fwd::Allocator>);
  static_assert(kComplete<fwd::Buffer>);
  static_assert(kComplete<fwd::CommandBatch>);
  static_assert(kComplete<fwd::Device>);
  static_assert(kComplete<fwd::GpuStageScope>);
  static_assert(kComplete<fwd::Image>);
  static_assert(kComplete<fwd::StorageInput>);
}

}  // namespace
