// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file query_pool.hpp
/// @brief A `VkQueryPool` and the commands that reset, write and read it.

#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

/// @brief Owns a `VkQueryPool` of timestamp (or other) queries.
///
/// The commands it records check their range and do nothing on an empty pool,
/// so a moved-from pool cannot pass a null handle to the driver. @ref GpuTimer
/// times compute work on one; gfx's profiler times frames on its own.
///
/// @warning The device passed to @ref create must outlive the pool.
///
/// @code
/// VKC_ASSIGN(QueryPool pool, QueryPool::create(device.handle(), 2));
/// pool.cmd_reset(cmd, 0, 2);
/// pool.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
/// // ... work ...
/// pool.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
/// // after the fence:
/// std::uint64_t ticks[2];
/// VKC_TRY(pool.read_results(0, 2, ticks));
/// @endcode
class VKC_VULKAN_API QueryPool {
 public:
  /// @brief Create a pool of @p query_count queries of @p type.
  /// @param device       The device.
  /// @param query_count  How many; non-zero.
  /// @param type         The query type.
  /// @return The pool; @ref Status::Code::InvalidArgument for a null
  ///         @p device or a zero count; or a backend @ref Status.
  static Result<QueryPool> create(VkDevice device, std::uint32_t query_count,
                                  VkQueryType type = VK_QUERY_TYPE_TIMESTAMP);

  /// @brief Construct an empty pool; @ref valid is false.
  QueryPool() noexcept = default;
  ~QueryPool() = default;
  QueryPool(QueryPool&& other) noexcept;
  QueryPool& operator=(QueryPool&& other) noexcept;
  QueryPool(const QueryPool&) = delete;
  QueryPool& operator=(const QueryPool&) = delete;

  /// @return The pool (`VK_NULL_HANDLE` when empty).
  VkQueryPool handle() const noexcept { return handle_.get(); }
  /// @return How many queries it holds (0 when empty).
  std::uint32_t query_count() const noexcept { return query_count_; }
  /// @return Whether this owns a pool.
  bool valid() const noexcept { return handle_.valid(); }

  /// @brief Record a reset of queries [@p first, @p first + @p count) into
  ///        @p cmd, where it is ordered against the writes that follow.
  ///        Nothing is recorded for an empty pool or a range past it.
  /// @param cmd    A recording command buffer, outside a render pass.
  /// @param first  The first query.
  /// @param count  How many.
  void cmd_reset(VkCommandBuffer cmd, std::uint32_t first,
                 std::uint32_t count) const noexcept;

  /// @brief Record a timestamp write of query @p index once @p stage is done.
  ///        Nothing is recorded for an empty pool or an index past it.
  /// @param cmd    A recording command buffer.
  /// @param stage  The stage the timestamp waits for.
  /// @param index  The query.
  void cmd_write_timestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage,
                           std::uint32_t index) const noexcept;

  /// @brief Read queries [@p first, @p first + @p count) back, never waiting.
  ///
  /// Read after the submission has retired: an unwritten query is a caller
  /// error. Without @p with_availability that is a `VK_NOT_READY` backend
  /// @ref Status; with it, each query's availability word says so and the
  /// call succeeds.
  /// @param first              The first query.
  /// @param count              How many.
  /// @param out                @p count 64-bit values, or `2 * count` --
  ///                           each value, then its availability (non-zero
  ///                           when written) -- with @p with_availability.
  /// @param with_availability  Whether to read availability words.
  /// @return OK; @ref Status::Code::InvalidArgument for an empty pool, a null
  ///         @p out or a range past the pool; or a backend @ref Status.
  Status read_results(std::uint32_t first, std::uint32_t count,
                      std::uint64_t* out, bool with_availability = false) const;

 private:
  UniqueHandle<VkQueryPool, vkDestroyQueryPool> handle_;
  std::uint32_t query_count_ = 0;
};

}  // namespace volumetric_kit::core
