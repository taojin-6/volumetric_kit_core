// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file gpu_timer.hpp
/// @brief Timestamp spans around GPU work -- what the device spent, apart
///        from the wall clock around a blocking submit -- and the scope that
///        publishes a stage's host and device time together.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/query_pool.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Device;

/// @brief Elapsed ticks between two timestamps, correct across a counter wrap.
///
/// Only the low @p valid_bits of a timestamp count, so the counter is an
/// N-bit ring and the span is `(end - begin) mod 2^N`; as `2^N` divides
/// `2^64`, the wrapped 64-bit subtraction is already congruent and one mask
/// recovers it. Masking the endpoints before subtracting would leave the
/// result modulo `2^64` -- thousands of years, across a wrap.
///
/// @code
/// const double ms = ticks_to_ms(timestamp_delta(t0, t1, valid_bits), period);
/// @endcode
/// @param begin       The earlier timestamp.
/// @param end         The later timestamp.
/// @param valid_bits  The queue family's `timestampValidBits`: 0 yields 0, 64
///                    or more the full counter.
/// @return `(end - begin)` modulo `2^valid_bits`.
constexpr std::uint64_t timestamp_delta(std::uint64_t begin, std::uint64_t end,
                                        std::uint32_t valid_bits) noexcept {
  if (valid_bits == 0) return 0;
  const std::uint64_t mask = valid_bits >= 64
                                 ? ~std::uint64_t{0}
                                 : (std::uint64_t{1} << valid_bits) - 1;
  return (end - begin) & mask;
}

/// @brief Ticks to milliseconds, in `double`, so a long span cannot overflow
///        a 64-bit nanosecond intermediate.
/// @param ticks                A tick delta (see @ref timestamp_delta).
/// @param timestamp_period_ns  Nanoseconds a tick (`limits.timestampPeriod`).
/// @return The span in milliseconds; 0 for a period that is not positive.
constexpr double ticks_to_ms(std::uint64_t ticks,
                             float timestamp_period_ns) noexcept {
  if (timestamp_period_ns <= 0.0F) return 0.0;
  return static_cast<double>(ticks) * static_cast<double>(timestamp_period_ns) *
         1e-6;
}

/// @brief Records timestamp spans into command buffers and resolves them to
///        milliseconds once their submits have completed.
///
/// Wall clock around a blocking submit folds recording, submission, the fence
/// stall and the device's work into one number, so "the kernel is slow" cannot
/// be told from "we are waiting"; this separates them. Every dispatch through
/// a @ref CommandBatch is fence-blocked, so its spans are readable the moment
/// the submit returns: no per-frame ring, no lag. A render loop that runs
/// frames ahead keeps its own ring (gfx's profiler) on a @ref QueryPool.
///
/// **Unavailable is not an error.** A queue family may have no timestamps
/// (`timestampValidBits` 0: MoltenVK on some configurations, a compute-only
/// family on some discrete GPUs), and a pool may fail to allocate. @ref create
/// still succeeds and @ref available is false: @ref begin returns
/// @ref kNoSpan, and @ref report_into adds no device rows, so a caller writes
/// the same code either way. A diagnostic must not be able to fail the work
/// it measures; @ref create refuses only a caller's mistake.
///
/// **A window has one owner: @ref report_into publishes it and ends it.**
/// Spans accumulate across submits until then, so several dispatches time
/// into one frame. Ending the window there keeps `max_spans` a per-frame bound
/// and keeps a create-once timer from republishing earlier frames.
///
/// @warning The device passed to @ref create must outlive the timer.
/// @warning Not synchronized: one timer records into one command buffer at a
///          time, on one thread.
///
/// @code
/// VKC_ASSIGN(GpuTimer timer, GpuTimer::create(device));  // once
/// for (;;) {
///   StageMetrics metrics;
///   {
///     GpuStageScope stage(&metrics, timer, "integrate");
///     VKC_TRY(batch.dispatch(kernel, &push, sizeof(push), groups, max,
///                            &stage));
///     VKC_TRY(batch.submit());
///   }  // host and device rows land in metrics
/// }
/// @endcode
class VKC_VULKAN_API GpuTimer {
 public:
  /// What @ref begin returns when no span was started: timing unavailable, or
  /// the window full.
  static constexpr std::uint32_t kNoSpan = 0xFFFFFFFFU;

  /// The ceiling on @ref create's `max_spans`: 4096 queries, two a span.
  /// MoltenVK backs a timestamp pool with a Metal counter sample buffer of at
  /// most 32 KiB -- 4096 timestamps -- and silently emulates timing for a
  /// larger one. Far above a window's real need (recon records a span a
  /// dispatch), and it also keeps the doubled count from wrapping.
  static constexpr std::uint32_t kMaxSpans = 2048U;

  /// @brief Construct an empty timer; @ref valid is false.
  GpuTimer() noexcept = default;

  /// @brief Create a timer for @p device's queue family.
  /// @param device     Supplies the family's timestamp bits and period; it
  ///                   must outlive the timer.
  /// @param max_spans  The most spans in one window, in [1, @ref kMaxSpans].
  /// @return The timer -- with @ref available false, and the reason logged,
  ///         where the family has no timestamps or the pool will not
  ///         allocate; or @ref Status::Code::InvalidArgument for an empty
  ///         @p device or a @p max_spans out of range.
  static Result<GpuTimer> create(const Device& device,
                                 std::uint32_t max_spans = 32);

  ~GpuTimer() = default;
  GpuTimer(GpuTimer&& other) noexcept;
  GpuTimer& operator=(GpuTimer&& other) noexcept;
  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  /// @return Whether the timer can time; when false, every call below is a
  ///         well-defined no-op.
  bool available() const noexcept { return valid_bits_ != 0 && pool_.valid(); }
  /// @return Whether this owns a timer (false when moved-from). A timer on a
  ///         device without timestamps is still valid, just not available.
  bool valid() const noexcept { return device_ != VK_NULL_HANDLE; }

  /// @brief Open a span: reset its two queries and write its start into
  ///        @p cmd.
  /// @param cmd   A recording command buffer, outside a render pass.
  /// @param name  The span's label, on @ref StageRow::name's terms; null
  ///              labels it `"gpu"`.
  /// @return The span's id for @ref end, or @ref kNoSpan when unavailable or
  ///         the window is full (logged once a window).
  std::uint32_t begin(VkCommandBuffer cmd, const char* name);

  /// @brief Close a span: write its end into @p cmd.
  /// @param cmd   The command buffer @ref begin recorded into.
  /// @param span  What @ref begin returned; @ref kNoSpan does nothing.
  void end(VkCommandBuffer cmd, std::uint32_t span);

  /// @brief Drop @p span, whose command buffer will not run: its queries are
  ///        unwritten, and reading them would be undefined -- or worse, an
  ///        earlier window's value, read under this span's label.
  ///
  /// Only the newest unresolved span can be dropped -- what a submit that
  /// failed before reaching the device leaves; anything else is ignored.
  /// @param span  What @ref begin returned.
  void discard(std::uint32_t span) noexcept;

  /// @brief Stop timing for good: a submit carrying this window's queries
  ///        may still run on the device.
  ///
  /// Its queries cannot be reused while it may still write them
  /// (VUID-vkCmdResetQueryPool-None-02841), and nothing says when it drains,
  /// so the pool retires with it: @ref available is false from here on.
  void abandon() noexcept;

  /// @brief Read back the spans recorded since the last resolve, and convert
  ///        them to milliseconds.
  ///
  /// Never blocks: a span whose queries are not written reads as unmeasured
  /// and @ref report_into skips it.
  /// @pre The submits carrying those spans have completed (their fences
  ///      signalled). An early read cannot be detected: a reused query still
  ///      holds the previous window's value. @ref CommandBatch::submit and
  ///      the @ref Device::submit_single_time overload sequence this behind
  ///      the fence.
  /// @return OK, or a backend @ref Status if the read failed.
  Status resolve();

  /// @brief Publish each resolved span to @p out as a device row, and end the
  ///        window.
  ///
  /// An unresolved span is skipped, not reported as 0: a missing row means
  /// "not measured", 0.00 "measured, and fast".
  /// @param out  The metrics the rows land in.
  void report_into(StageMetrics& out);

  /// @brief Discard the window's spans without publishing them.
  void reset() noexcept { end_window(); }

  /// @return How many spans the window holds.
  std::size_t count() const noexcept { return spans_.size(); }

  /// @brief Raise the per-window bound to at least @p max_spans (clamped to
  ///        @ref kMaxSpans), for a caller whose window grows with its input.
  ///        Does nothing within a window or on an unavailable timer.
  /// @param device     The device the timer was made for.
  /// @param max_spans  The bound wanted.
  /// @return OK, or @ref create's failure.
  Status reserve(const Device& device, std::uint32_t max_spans);

 private:
  struct Span {
    const char* name = nullptr;  // on StageRow::name's terms
    double ms = 0.0;
    bool resolved = false;
  };

  void clear_state() noexcept;
  // Ends the window, and re-arms the full-window warning for the next.
  void end_window() noexcept {
    spans_.clear();
    warned_full_ = false;
  }

  VkDevice device_ = VK_NULL_HANDLE;
  QueryPool pool_;
  std::uint32_t max_spans_ = 0;
  std::uint32_t valid_bits_ = 0;
  float period_ns_ = 0.0F;
  bool warned_full_ = false;
  std::vector<Span> spans_;
  // Reserved at create, so recording and resolving a span allocate nothing.
  std::vector<std::uint64_t> readback_;
};

/// @brief One stage's window: times its host span, hands its device timer to
///        the work it records, and publishes both halves when it closes.
///
/// One object, because a stage that opens a host scope, threads its timer
/// into a dispatch and publishes as a statement afterwards skips the publish
/// on every early return -- and its spans stay in the timer, to be published
/// under the next call's label. A destructor cannot be skipped. It is also
/// what a dispatch takes, so a timer cannot reach one without its label.
///
/// Null metrics make it inert end to end: @ref timer is null, the work takes
/// the untimed path, and nothing is published.
///
/// @warning The metrics, the timer and the @p name literal must outlive the
///          scope.
///
/// @code
/// Status Integrator::integrate(const Frame& frame, StageMetrics* metrics) {
///   GpuStageScope stage(metrics, timer_, "integrate");  // every return
///   CommandBatch batch(*device_, *allocator_);          // publishes
///   VKC_TRY(batch.dispatch(kernel_, &push, sizeof(push), groups, max,
///                          &stage));
///   return batch.submit();
/// }
/// @endcode
class GpuStageScope {
 public:
  /// @brief Open @p name's window on @p metrics, with device spans in
  ///        @p timer; inert when @p metrics is null.
  /// @param metrics  The set both halves land in, or null.
  /// @param timer    The timer device spans record into.
  /// @param name     The label; string-literal lifetime.
  GpuStageScope(StageMetrics* metrics, GpuTimer& timer, const char* name)
      : metrics_(metrics), timer_(&timer), host_(metrics, name) {
    // Seeded first, so the row precedes any breakdown row the stage adds.
    if (metrics_ != nullptr) metrics_->seed(name);
  }

  /// @brief Publish the device spans, then close the host row: this body runs
  ///        before @ref host_, the last member, is destroyed.
  // As StageScope's destructor: a row's allocation failing terminates.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~GpuStageScope() {
    if (metrics_ != nullptr) timer_->report_into(*metrics_);
  }

  GpuStageScope(const GpuStageScope&) = delete;
  GpuStageScope& operator=(const GpuStageScope&) = delete;
  GpuStageScope(GpuStageScope&&) = delete;
  GpuStageScope& operator=(GpuStageScope&&) = delete;

  /// @return The timer to record into, or null when inert -- the untimed
  ///         path.
  GpuTimer* timer() const noexcept {
    return metrics_ != nullptr ? timer_ : nullptr;
  }
  /// @return The label device spans publish under.
  const char* name() const noexcept { return host_.name(); }

 private:
  StageMetrics* metrics_;
  GpuTimer* timer_;
  StageScope host_;
};

}  // namespace volumetric_kit::core
