// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file buffer_readback.hpp
/// @brief A device-local buffer's contents on the host and back, for a test,
///        through a @ref volumetric_kit::core::CommandBatch, with no test
///        framework (`volumetric_kit::core_test_policy`).
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// const std::vector<std::uint32_t> words = {1, 2, 3};
/// VKC_ASSIGN(Buffer buffer,
///            test::upload_device_buffer(device, allocator, words.data(),
///                                       words.size() * sizeof(words[0])));
/// VKC_TRY(test::write_back(device, allocator, buffer,
///                          std::vector<std::uint32_t>{7}, 1));
/// VKC_ASSIGN(std::vector<std::uint32_t> back,
///            test::read_back<std::uint32_t>(device, allocator, buffer, 3));
/// // back == {1, 7, 3}
/// @endcode

#include <cstddef>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

/// @brief The device and allocator a test runs on, handed to its helpers.
///
/// Held by reference: the test keeps both where it created them.
///
/// @code
/// test::Gpu gpu{device, allocator};
/// VKC_ASSIGN(std::vector<float> sdf,
///            test::read_back<float>(gpu.device, gpu.allocator, buffer, n));
/// @endcode
struct Gpu {
  Device& device;        ///< The device.
  Allocator& allocator;  ///< The allocator on it.
};

/// @brief A genuinely resident input, uploaded at the test boundary.
/// @param device     The queue it uploads on.
/// @param allocator  Makes the buffer and its staging, on @p device.
/// @param data       The bytes.
/// @param bytes      Their size.
/// @return A device-local storage buffer holding @p data, with transfer
///         usage both ways (@ref device_storage_buffer); or the allocation's
///         or the submit's failure.
inline Result<Buffer> upload_device_buffer(const Device& device,
                                           Allocator& allocator,
                                           const void* data,
                                           VkDeviceSize bytes) {
  VKC_ASSIGN(Buffer buffer, device_storage_buffer(allocator, bytes));
  CommandBatch batch(device, allocator);
  VKC_TRY(batch.upload(buffer, 0, data, bytes));
  VKC_TRY(batch.submit());
  return buffer;
}

/// @brief Read elements of a buffer back to the host.
/// @tparam T         The element type.
/// @param device     The queue it reads on.
/// @param allocator  Makes the staging, on @p device.
/// @param buffer     A buffer with `TRANSFER_SRC` usage.
/// @param count      How many elements.
/// @param first      The first element.
/// @return @p count elements of @p buffer from element @p first; or the
///         readback's failure.
template <typename T>
Result<std::vector<T>> read_back(const Device& device, Allocator& allocator,
                                 const Buffer& buffer, std::size_t count,
                                 std::size_t first = 0) {
  std::vector<T> out(count);
  CommandBatch batch(device, allocator);
  VKC_TRY(batch.readback(buffer, static_cast<VkDeviceSize>(first) * sizeof(T),
                         static_cast<VkDeviceSize>(count) * sizeof(T),
                         out.data()));
  VKC_TRY(batch.submit());
  return out;
}

/// @brief Write elements over a buffer.
/// @tparam T         The element type.
/// @param device     The queue it writes on.
/// @param allocator  Makes the staging, on @p device.
/// @param buffer     A buffer with `TRANSFER_DST` usage.
/// @param data       The elements.
/// @param first      The element @p data's first lands on.
/// @return OK; or the upload's or the submit's failure.
template <typename T>
Status write_back(const Device& device, Allocator& allocator,
                  const Buffer& buffer, const std::vector<T>& data,
                  std::size_t first = 0) {
  CommandBatch batch(device, allocator);
  VKC_TRY(batch.upload(buffer, static_cast<VkDeviceSize>(first) * sizeof(T),
                       data.data(),
                       static_cast<VkDeviceSize>(data.size()) * sizeof(T)));
  return batch.submit();
}

}  // namespace volumetric_kit::core::test
