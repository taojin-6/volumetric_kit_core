// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/gpu_timer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/query_pool.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

constexpr const char* kLogSource = "vulkan";

}  // namespace

GpuTimer::GpuTimer(GpuTimer&& other) noexcept
    : device_(other.device_),
      pool_(std::move(other.pool_)),
      max_spans_(other.max_spans_),
      valid_bits_(other.valid_bits_),
      period_ns_(other.period_ns_),
      warned_full_(other.warned_full_),
      spans_(std::move(other.spans_)),
      readback_(std::move(other.readback_)) {
  other.clear_state();
}

GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
  if (this != &other) {
    device_ = other.device_;
    pool_ = std::move(other.pool_);  // frees this timer's pool first
    max_spans_ = other.max_spans_;
    valid_bits_ = other.valid_bits_;
    period_ns_ = other.period_ns_;
    warned_full_ = other.warned_full_;
    spans_ = std::move(other.spans_);
    readback_ = std::move(other.readback_);
    other.clear_state();
  }
  return *this;
}

// Every member back to its default, so a moved-from timer is an empty one.
void GpuTimer::clear_state() noexcept {
  device_ = VK_NULL_HANDLE;
  max_spans_ = 0;
  valid_bits_ = 0;
  period_ns_ = 0.0F;
  warned_full_ = false;
  spans_.clear();
  readback_.clear();
}

Result<GpuTimer> GpuTimer::create(const Device& device,
                                  std::uint32_t max_spans) {
  if (device.handle() == VK_NULL_HANDLE) {
    return Status::invalid_argument("GpuTimer::create: device is empty");
  }
  if (max_spans == 0 || max_spans > kMaxSpans) {
    return Status::invalid_argument(
        "GpuTimer::create: max_spans must be in [1, kMaxSpans]");
  }
  GpuTimer timer;
  timer.device_ = device.handle();
  timer.max_spans_ = max_spans;
  // Per family, not per device: timestampComputeAndGraphics covers only the
  // families that do both, and a library may be handed a compute-only one.
  timer.valid_bits_ = device.timestamp_valid_bits();
  timer.period_ns_ = device.caps().limits().timestampPeriod;
  if (timer.valid_bits_ == 0 || timer.period_ns_ <= 0.0F) {
    return timer;  // valid, not available
  }
  Result<QueryPool> pool = QueryPool::create(device.handle(), max_spans * 2);
  if (!pool) {
    // Degrade, as for a family with no timestamps: a library makes its timer
    // in its own create(), which must not fail for a caller who never asked
    // for timing. Logged, so the missing device rows have a stated reason.
    log_message(LogLevel::Warning, kLogSource,
                "GpuTimer::create: " + pool.status().message() +
                    "; device timings are unavailable");
    timer.valid_bits_ = 0;
    return timer;
  }
  timer.pool_ = *std::move(pool);
  timer.spans_.reserve(max_spans);
  timer.readback_.reserve(static_cast<std::size_t>(max_spans) * 4);
  return timer;
}

std::uint32_t GpuTimer::begin(VkCommandBuffer cmd, const char* name) {
  if (!available() || cmd == VK_NULL_HANDLE) return kNoSpan;
  if (spans_.size() >= max_spans_) {
    // A full window refusing silently freezes a GPU column on stale values.
    if (!warned_full_) {
      warned_full_ = true;
      log_message(LogLevel::Warning, kLogSource,
                  "GpuTimer: the window is full (max_spans); further spans go "
                  "untimed until report_into or reset ends it");
    }
    return kNoSpan;
  }
  const auto span = static_cast<std::uint32_t>(spans_.size());
  spans_.push_back(Span{name != nullptr ? name : "gpu", 0.0, false});
  // Reset this span's pair in the command buffer, not the whole pool up
  // front: a query must not be reset while in flight, and spans recorded into
  // an earlier command buffer of the window stay readable.
  pool_.cmd_reset(cmd, span * 2, 2);
  // BOTTOM_OF_PIPE for the start too: TOP_OF_PIPE orders against nothing
  // before it, so spans in one command buffer would each latch near its front
  // and nest rather than tile -- and on a shared device latch while another
  // library's work drains, charging its tail here.
  pool_.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            span * 2);
  return span;
}

void GpuTimer::end(VkCommandBuffer cmd, std::uint32_t span) {
  if (!available() || span == kNoSpan || span >= spans_.size()) return;
  pool_.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            (span * 2) + 1);
}

void GpuTimer::discard(std::uint32_t span) noexcept {
  // Only the newest: an earlier span's queries are indexed by position, so
  // removing it would renumber every span after it.
  if (span == kNoSpan || spans_.empty() ||
      static_cast<std::size_t>(span) + 1 != spans_.size() ||
      spans_.back().resolved) {
    return;
  }
  spans_.pop_back();
}

void GpuTimer::abandon() noexcept {
  // available() reads valid_bits_, so this retires the pool while the handle
  // is still freed on destruction.
  valid_bits_ = 0;
  end_window();
}

Status GpuTimer::resolve() {
  if (!available()) return {};
  // Only the unresolved tail: a batch resolves after every submit, and
  // re-reading the window each time would grow quadratically with it.
  std::size_t first = 0;
  while (first < spans_.size() && spans_[first].resolved) ++first;
  if (first == spans_.size()) return {};
  const auto first_query = static_cast<std::uint32_t>(first * 2);
  const auto queries = static_cast<std::uint32_t>((spans_.size() - first) * 2);
  readback_.assign(static_cast<std::size_t>(queries) * 2, 0);
  VKC_TRY(pool_.read_results(first_query, queries, readback_.data(),
                             /*with_availability=*/true));
  for (std::size_t i = first; i < spans_.size(); ++i) {
    const std::size_t at = (i - first) * 4;
    // An unwritten end -- a span begun and never ended -- stays unresolved.
    if (readback_[at + 1] == 0 || readback_[at + 3] == 0) continue;
    spans_[i].ms = ticks_to_ms(
        timestamp_delta(readback_[at], readback_[at + 2], valid_bits_),
        period_ns_);
    spans_[i].resolved = true;
  }
  return {};
}

void GpuTimer::report_into(StageMetrics& out) {
  for (const Span& span : spans_) {
    if (span.resolved) out.add_gpu(span.name, span.ms);
  }
  end_window();
}

Status GpuTimer::reserve(const Device& device, std::uint32_t max_spans) {
  max_spans = std::min(max_spans, kMaxSpans);
  if (!available() || max_spans <= max_spans_ || !spans_.empty()) return {};
  VKC_ASSIGN(*this, create(device, max_spans));
  return {};
}

}  // namespace volumetric_kit::core
