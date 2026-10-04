// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/gpu_timer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/query_pool.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

constexpr const char* kLogSource = "vulkan";
// Scopes on one timer nest a few deep, so opening one allocates nothing.
constexpr std::size_t kScopesReserved = 8;

}  // namespace

GpuTimer::GpuTimer(GpuTimer&& other) noexcept
    : device_(other.device_),
      pool_(std::move(other.pool_)),
      max_spans_(other.max_spans_),
      valid_bits_(other.valid_bits_),
      period_ns_(other.period_ns_),
      warned_full_(other.warned_full_),
      warned_closed_(other.warned_closed_),
      spans_(std::move(other.spans_)),
      scopes_(std::move(other.scopes_)),
      next_scope_(other.next_scope_),
      readback_(std::move(other.readback_)) {
  other.clear_state();
}

GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
  if (this != &other) {
    device_ = other.device_;
    // Drops this timer's pool, unless a submit left to the device holds it.
    pool_ = std::move(other.pool_);
    max_spans_ = other.max_spans_;
    valid_bits_ = other.valid_bits_;
    period_ns_ = other.period_ns_;
    warned_full_ = other.warned_full_;
    warned_closed_ = other.warned_closed_;
    spans_ = std::move(other.spans_);
    scopes_ = std::move(other.scopes_);
    next_scope_ = other.next_scope_;
    readback_ = std::move(other.readback_);
    other.clear_state();
  }
  return *this;
}

// Every member back to its default, so a moved-from timer is an empty one.
void GpuTimer::clear_state() noexcept {
  device_ = VK_NULL_HANDLE;
  pool_.reset();
  max_spans_ = 0;
  valid_bits_ = 0;
  period_ns_ = 0.0F;
  warned_full_ = false;
  warned_closed_ = false;
  spans_.clear();
  scopes_.clear();
  next_scope_ = 1;
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
  timer.scopes_.reserve(kScopesReserved);  // scopes open on any timer
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
  timer.pool_ = std::make_shared<QueryPool>(*std::move(pool));
  timer.spans_.reserve(max_spans);
  timer.readback_.reserve(static_cast<std::size_t>(max_spans) * 4);
  return timer;
}

std::uint32_t GpuTimer::begin(VkCommandBuffer cmd, const GpuSpanTag& tag,
                              bool reset) {
  VKC_CHECK(tag.name != nullptr, "GpuTimer::begin: the span's name is null");
  if (!available() || cmd == VK_NULL_HANDLE) return kNoSpan;
  OpenScope* scope = nullptr;
  if (tag.scope != 0) {
    scope = find_scope(tag.scope);
    if (scope == nullptr) {
      // Recorded under a scope that has since closed: nothing is left to
      // publish the span, which would otherwise hold its slot to the end of
      // the window.
      if (!warned_closed_) {
        warned_closed_ = true;
        log_message(LogLevel::Warning, kLogSource,
                    "GpuTimer: a command's GpuStageScope closed before its "
                    "submit; the command runs untimed");
      }
      return kNoSpan;
    }
  }
  if (spans_.size() >= max_spans_) {
    // A full window refusing silently freezes a GPU column on stale values;
    // publishing what fit would pass part of a stage off as all of it.
    if (scope != nullptr) scope->truncated = true;
    if (!warned_full_) {
      warned_full_ = true;
      log_message(LogLevel::Warning, kLogSource,
                  "GpuTimer: the window is full (max_spans); further spans go "
                  "untimed, and a stage that loses one reports no device "
                  "time, until the window ends");
    }
    return kNoSpan;
  }
  const auto span = static_cast<std::uint32_t>(spans_.size());
  spans_.push_back(Span{tag.name, tag.scope, 0.0, false});
  // This span's pair in the command buffer, not the whole pool up front: a
  // query must not be reset while in flight, and spans recorded into an
  // earlier command buffer of the window stay readable.
  if (reset) pool_->cmd_reset(cmd, span * 2, 2);
  // BOTTOM_OF_PIPE for the start too: TOP_OF_PIPE orders against nothing
  // before it, so spans in one command buffer would each latch near its front
  // and nest rather than tile -- and on a shared device latch while another
  // library's work drains, charging its tail here.
  pool_->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             span * 2);
  return span;
}

void GpuTimer::cmd_reset_ahead(VkCommandBuffer cmd,
                               std::uint32_t spans) const noexcept {
  const auto first = static_cast<std::uint32_t>(spans_.size());
  if (!available() || first >= max_spans_) return;
  const std::uint32_t count = std::min(spans, max_spans_ - first);
  if (count != 0) pool_->cmd_reset(cmd, first * 2, count * 2);
}

void GpuTimer::end(VkCommandBuffer cmd, std::uint32_t span) {
  if (!available() || span == kNoSpan || span >= spans_.size()) return;
  pool_->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             (span * 2) + 1);
}

void GpuTimer::settle(std::uint32_t first, std::uint32_t count,
                      const Status& submitted, bool in_flight) {
  if (!submitted.ok()) {
    // Work the device may still run may still write every query it reset or
    // wrote, so none can be reused; work that never reached it wrote none.
    if (in_flight) {
      abandon();
    } else {
      discard(first, count);
    }
    return;
  }
  if (count == 0) return;
  // Logged, not returned: the work succeeded, and failing it over a
  // diagnostic is what this class exists not to do. The spans stay
  // unresolved, which publishes as "not measured".
  const Status resolved = resolve(first, count);
  if (!resolved.ok()) {
    log_message(LogLevel::Warning, kLogSource,
                "GpuTimer: timestamps not resolved (" + resolved.message() +
                    "); the work itself succeeded");
  }
}

Status GpuTimer::resolve(std::uint32_t first, std::uint32_t count) {
  if (!available() || count == 0) return {};
  if (first >= spans_.size() || count > spans_.size() - first) {
    return Status::invalid_argument("GpuTimer::resolve: spans past the window");
  }
  readback_.assign(static_cast<std::size_t>(count) * 4, 0);
  VKC_TRY(pool_->read_results(first * 2, count * 2, readback_.data(),
                              /*with_availability=*/true));
  for (std::size_t i = 0; i < count; ++i) {
    Span& span = spans_[first + i];
    const std::size_t at = i * 4;
    // An unwritten end -- a span begun and never ended -- stays unresolved.
    if (span.resolved || readback_[at + 1] == 0 || readback_[at + 3] == 0) {
      continue;
    }
    span.ms = ticks_to_ms(
        timestamp_delta(readback_[at], readback_[at + 2], valid_bits_),
        period_ns_);
    span.resolved = true;
  }
  return {};
}

void GpuTimer::discard(std::uint32_t first, std::uint32_t count) noexcept {
  if (count == 0 || first >= spans_.size() || spans_.size() - first != count) {
    return;
  }
  for (std::size_t i = first; i < spans_.size(); ++i) {
    if (spans_[i].resolved) return;
  }
  while (spans_.size() > first) spans_.pop_back();
}

void GpuTimer::abandon() noexcept {
  // available() reads valid_bits_, so this retires the pool, which a submit
  // left to the device still holds; the window keeps what it resolved.
  valid_bits_ = 0;
}

void GpuTimer::report_into(StageMetrics& out) {
  for (const Span& span : spans_) {
    if (span.resolved) out.add_gpu(span.name, span.ms);
  }
  end_window();
}

std::uint64_t GpuTimer::open_scope() {
  const std::uint64_t id = next_scope_++;
  scopes_.push_back(OpenScope{id, false});
  return id;
}

void GpuTimer::close_scope(std::uint64_t id, StageMetrics& out) {
  // From the back: scopes nest, so it is the innermost open one.
  const auto open =
      std::find_if(scopes_.rbegin(), scopes_.rend(),
                   [id](const OpenScope& scope) { return scope.id == id; });
  if (open == scopes_.rend()) return;  // the timer was moved or replaced
  const bool truncated = open->truncated;
  scopes_.erase(std::next(open).base());
  if (!truncated) {
    for (const Span& span : spans_) {
      if (span.scope == id && span.resolved) out.add_gpu(span.name, span.ms);
    }
  }
  // Every span of a closed scope is published or never will be: one still
  // unresolved never ran, or ran on a submit that failed.
  if (scopes_.empty()) end_window();
}

GpuTimer::OpenScope* GpuTimer::find_scope(std::uint64_t id) noexcept {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (it->id == id) return &*it;
  }
  return nullptr;
}

void GpuTimer::end_window() noexcept {
  spans_.clear();
  warned_full_ = false;
  warned_closed_ = false;
  // A scope still open starts the next window with nothing refused.
  for (OpenScope& scope : scopes_) scope.truncated = false;
}

Status GpuTimer::reserve(const Device& device, std::uint32_t max_spans) {
  if (valid() && device.handle() != device_) {
    return Status::invalid_argument(
        "GpuTimer::reserve: not the device the timer was made for");
  }
  const std::uint32_t wanted = std::min(max_spans, kMaxSpans);
  if (!available() || wanted <= max_spans_ || !spans_.empty()) return {};
  Result<QueryPool> pool = QueryPool::create(device_, wanted * 2);
  if (!pool) {
    // A larger pool failing is no reason to stop timing at the size that
    // works: the timer keeps its own, and its bound.
    log_message(LogLevel::Warning, kLogSource,
                "GpuTimer::reserve: " + pool.status().message() + "; keeping " +
                    std::to_string(max_spans_) + " spans a window");
    return {};
  }
  if (max_spans > kMaxSpans) {
    log_message(LogLevel::Warning, kLogSource,
                "GpuTimer::reserve: " + std::to_string(max_spans) +
                    " spans asked, " + std::to_string(kMaxSpans) +
                    " kept (kMaxSpans); a stage that fills the window reports "
                    "no device time");
  }
  spans_.reserve(wanted);
  readback_.reserve(static_cast<std::size_t>(wanted) * 4);
  pool_ = std::make_shared<QueryPool>(*std::move(pool));
  max_spans_ = wanted;
  return {};
}

}  // namespace volumetric_kit::core
