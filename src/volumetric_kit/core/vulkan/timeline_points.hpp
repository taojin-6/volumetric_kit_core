// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_vulkan: the timeline values a submit waits for and sets,
// checked alike by Device::submit_pending and by CommandBatch::submit_async,
// which checks them before it marks its batch submitted; and the requirement
// TimelineSemaphore::create checks, which the submits check too. Not
// installed.

#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"

namespace volumetric_kit::core::detail {

// What a timeline semaphore needs of a device: the feature alone, on any
// queue, at the version the feature itself needs.
DeviceRequirements timeline_requirements();

// The refusals a submit makes before it takes a command buffer, each naming
// `call`: a null or empty semaphore; one made on another VkDevice; any value
// on a device that did not enable timelineSemaphore; and a value to set that
// would not advance its counter -- one semaphore set twice in the submit, a
// value not above one the submit waits for on the same semaphore, or not
// above both the counter and every value an earlier submit sets.
Status check_timeline_points(const Device& device,
                             const std::vector<TimelinePoint>& wait,
                             const std::vector<TimelinePoint>& signal,
                             const char* call);

// Records that a submit which reached the queue sets each value in `signal`,
// so a later one must set a higher one.
void note_signals(const std::vector<TimelinePoint>& signal) noexcept;

}  // namespace volumetric_kit::core::detail
