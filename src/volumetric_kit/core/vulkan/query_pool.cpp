// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/query_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<QueryPool> QueryPool::create(VkDevice device, std::uint32_t query_count,
                                    VkQueryType type) {
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("QueryPool::create: device is null");
  }
  // VUID-VkQueryPoolCreateInfo-queryCount-02763.
  if (query_count == 0) {
    return Status::invalid_argument("QueryPool::create: query_count is zero");
  }
  VkQueryPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  info.queryType = type;
  info.queryCount = query_count;
  VkQueryPool handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateQueryPool(device, &info, nullptr, &handle));
  QueryPool pool;
  pool.handle_ = UniqueHandle<VkQueryPool, vkDestroyQueryPool>(device, handle);
  pool.query_count_ = query_count;
  return pool;
}

QueryPool::QueryPool(QueryPool&& other) noexcept
    : handle_(std::move(other.handle_)),
      query_count_(std::exchange(other.query_count_, 0)) {}

QueryPool& QueryPool::operator=(QueryPool&& other) noexcept {
  if (this != &other) {
    handle_ = std::move(other.handle_);  // frees this pool's handle first
    query_count_ = std::exchange(other.query_count_, 0);
  }
  return *this;
}

// An empty pool has a count of 0, so the range checks refuse it too.
void QueryPool::cmd_reset(VkCommandBuffer cmd, std::uint32_t first,
                          std::uint32_t count) const noexcept {
  if (cmd == VK_NULL_HANDLE || count == 0 || count > query_count_ ||
      first > query_count_ - count) {
    return;
  }
  vkCmdResetQueryPool(cmd, handle_.get(), first, count);
}

void QueryPool::cmd_write_timestamp(VkCommandBuffer cmd,
                                    VkPipelineStageFlagBits stage,
                                    std::uint32_t index) const noexcept {
  if (cmd == VK_NULL_HANDLE || index >= query_count_) return;
  vkCmdWriteTimestamp(cmd, stage, handle_.get(), index);
}

Status QueryPool::read_results(std::uint32_t first, std::uint32_t count,
                               std::uint64_t* out,
                               bool with_availability) const {
  if (!valid()) {
    return Status::invalid_argument("QueryPool::read_results: pool is empty");
  }
  if (out == nullptr || count == 0 || count > query_count_ ||
      first > query_count_ - count) {
    return Status::invalid_argument(
        "QueryPool::read_results: null destination, or a range past the "
        "pool");
  }
  // Never WAIT: read after the submission retired, a missing result is the
  // caller's error, which must not turn into a hang inside a diagnostic.
  const std::size_t words = with_availability ? 2 : 1;
  VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
  if (with_availability) flags |= VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
  const VkResult result =
      vkGetQueryPoolResults(handle_.device(), handle_.get(), first, count,
                            count * words * sizeof(std::uint64_t), out,
                            words * sizeof(std::uint64_t), flags);
  // With availability words, VK_NOT_READY only says some query is unwritten,
  // which those words report one by one.
  if (result == VK_SUCCESS || (with_availability && result == VK_NOT_READY)) {
    return {};
  }
  return vk_error(result, "vkGetQueryPoolResults");
}

}  // namespace volumetric_kit::core
