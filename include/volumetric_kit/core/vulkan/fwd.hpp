// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file fwd.hpp
/// @brief Forward declarations of the vulkan tier's classes that a sibling's
///        headers name by pointer or reference.
///
/// A header that only names a `Device`, `Buffer` or `CommandBatch` by pointer
/// or reference includes this rather than the full header, and so includes no
/// Vulkan. The classes are declared in the core's namespace: a `class Device;`
/// in a sibling's own namespace would declare an unrelated class.
///
/// @code
/// #include "volumetric_kit/core/vulkan/fwd.hpp"
///
/// core::Status record(core::CommandBatch& batch, const core::Device& device);
/// @endcode

namespace volumetric_kit::core {
class Allocator;      // allocator.hpp
class Buffer;         // buffer.hpp
class CommandBatch;   // command_batch.hpp
class Device;         // device.hpp
class GpuStageScope;  // gpu_timer.hpp
class Image;          // image.hpp
class StorageInput;   // compute_util.hpp
}  // namespace volumetric_kit::core
