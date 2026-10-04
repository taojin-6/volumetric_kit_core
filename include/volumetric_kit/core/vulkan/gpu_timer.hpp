// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file gpu_timer.hpp
/// @brief Timestamp spans around GPU work -- what the device spent, apart
///        from the wall clock around a blocking submit -- and the scope that
///        publishes a stage's host and device time together.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/query_pool.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class Device;
class GpuTimer;

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

/// @brief What a recorded command keeps of a @ref GpuStageScope until its
///        submit opens the span: the timer, the label, and the scope by id.
///
/// A value, so a scope that closes before the submit leaves nothing to
/// dangle: the timer refuses a span for a scope no longer open, and the
/// command runs untimed. The timer itself must still be there.
///
/// @code
/// const GpuSpanTag tag = stage.tag();  // when the command is recorded
/// // ... later, recording the command buffer:
/// const std::uint32_t span = tag.timer->begin(cmd, tag);
/// @endcode
struct GpuSpanTag {
  /// The timer the span records into; null is untimed.
  GpuTimer* timer = nullptr;
  /// The label, on @ref StageRow::name's terms.
  const char* name = nullptr;
  /// The scope that publishes the span, or 0 for a span
  /// @ref GpuTimer::report_into publishes.
  std::uint64_t scope = 0;
};

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
/// (`timestampValidBits` 0, as some implementations and some compute-only
/// families report), and a pool may fail to allocate. @ref create
/// still succeeds and @ref available is false: @ref begin returns
/// @ref kNoSpan, and nothing publishes a device row, so a caller writes the
/// same code either way. A diagnostic must not be able to fail the work it
/// measures; @ref create refuses only a caller's mistake.
///
/// **A span is read only after its own submit.** @ref settle resolves the
/// spans one submit carried and no others, so a submit nested inside another's
/// recording never reads the outer span, whose command buffer has not run.
///
/// **Spans belong to the scope that opened them.** A @ref GpuStageScope
/// publishes its own spans when it closes, so two scopes open on one timer --
/// a stage, and a helper with metrics of its own -- each publish theirs. Spans
/// accumulate across submits until the last open scope closes, which ends the
/// window: `max_spans` is a bound per outermost scope, a frame's stage. A
/// timer used by hand, without scopes, is published by @ref report_into,
/// which ends the window too; use a timer one way or the other.
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
///     CommandBatch batch(device, allocator);  // submits once
///     VKC_TRY(batch.dispatch(kernel, &push, sizeof(push), groups, max,
///                            &stage));
///     VKC_TRY(batch.submit());
///   }  // host and device rows land in metrics
/// }
/// @endcode
class VKC_VULKAN_API GpuTimer {
 public:
  /// What @ref begin returns when no span was started: timing unavailable,
  /// the window full, or the span's scope closed.
  static constexpr std::uint32_t kNoSpan = 0xFFFFFFFFU;

  /// The ceiling on @ref create's `max_spans`: 4096 queries, two a span.
  /// An implementation layered on Metal backs a timestamp pool with a counter
  /// sample buffer of at most 32 KiB -- 4096 timestamps -- and silently
  /// emulates timing for a larger one. Far above a window's real need (recon
  /// records a span a dispatch), and it also keeps the doubled count from
  /// wrapping.
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
  bool available() const noexcept {
    return valid_bits_ != 0 && pool_ != nullptr && pool_->valid();
  }
  /// @return Whether this owns a timer (false when moved-from). A timer on a
  ///         device without timestamps is still valid, just not available.
  bool valid() const noexcept { return device_ != VK_NULL_HANDLE; }

  /// @brief Open a span by hand, for @ref report_into to publish: reset its
  ///        two queries and write its start into @p cmd.
  /// @param cmd   A recording command buffer, outside a render pass.
  /// @param name  The span's label, on @ref StageRow::name's terms; not null
  ///              (`VKC_CHECK`).
  /// @return The span's id, or @ref kNoSpan when unavailable or the window is
  ///         full (logged once a window).
  std::uint32_t begin(VkCommandBuffer cmd, const char* name) {
    return begin(cmd, GpuSpanTag{this, name, 0});
  }

  /// @brief Open @p tag's span: reset its two queries, unless @p reset is
  ///        false, and write its start into @p cmd.
  /// @param cmd    A recording command buffer, outside a render pass.
  /// @param tag    The span's label, not null (`VKC_CHECK`), and its scope;
  ///               its timer is this one.
  /// @param reset  False only where @ref cmd_reset_ahead has reset the span's
  ///               queries earlier in @p cmd.
  /// @return The span's id, or @ref kNoSpan when unavailable, the window is
  ///         full, or @p tag's scope has closed (each logged once a window).
  ///         A scope that loses a span to a full window publishes no device
  ///         time: a partial sum would read as the whole stage's.
  std::uint32_t begin(VkCommandBuffer cmd, const GpuSpanTag& tag,
                      bool reset = true);

  /// @brief Reset, in one command, the queries of the next @p spans spans,
  ///        which a recorder then opens into @p cmd with @ref begin's
  ///        `reset` false: a batch of timed commands records one reset, not
  ///        one between every two commands.
  /// @param cmd    A recording command buffer, outside a render pass.
  /// @param spans  How many spans will follow; clamped to the window's room.
  void cmd_reset_ahead(VkCommandBuffer cmd, std::uint32_t spans) const noexcept;

  /// @brief Close a span: write its end into @p cmd.
  /// @param cmd   The command buffer @ref begin recorded into.
  /// @param span  What @ref begin returned; @ref kNoSpan does nothing.
  void end(VkCommandBuffer cmd, std::uint32_t span);

  /// @brief Settle spans [@p first, @p first + @p count) once the submit
  ///        that carried them has returned.
  ///
  /// Resolves them if the submit succeeded, logging a failed read rather
  /// than returning it, as the work itself succeeded; drops them
  /// (@ref discard) if the work never reached the device; and retires the
  /// timer (@ref abandon) if the device may still run it. What
  /// @ref CommandBatch::submit and the timed @ref Device::submit_single_time
  /// do; a caller timing its own submits does the same.
  /// @param first      The first span the submit carried.
  /// @param count      How many it carried; 0 settles none, but still retires
  ///                   the timer for work in flight.
  /// @param submitted  What the submit returned.
  /// @param in_flight  Whether the device may still run the work, as
  ///                   @ref Device::submit_single_time reports it.
  void settle(std::uint32_t first, std::uint32_t count, const Status& submitted,
              bool in_flight);

  /// @brief Read back spans [@p first, @p first + @p count) -- those one
  ///        completed submit carried -- and convert them to milliseconds.
  ///
  /// Only those: a span another command buffer carries may not have run, and
  /// a reused query would still hold the previous window's value. Never
  /// blocks: a span whose queries are not written stays unresolved, and is
  /// published as unmeasured.
  /// @pre The submit carrying the spans has completed (its fence signalled).
  /// @param first  The first span.
  /// @param count  How many.
  /// @return OK; @ref Status::Code::InvalidArgument for a range past the
  ///         window; or a backend @ref Status if the read failed.
  Status resolve(std::uint32_t first, std::uint32_t count = 1);

  /// @brief Drop spans [@p first, @p first + @p count), whose command buffer
  ///        will not run, so their slots go back to the window.
  ///
  /// Only the window's newest spans, unresolved: a span is its queries'
  /// position, so dropping an earlier run would renumber every span after
  /// it. A run that is not the newest -- an outer submit's, around a nested
  /// one -- stays, unresolved: nothing reads it, and nothing publishes it.
  /// @param first  The first span.
  /// @param count  How many.
  void discard(std::uint32_t first, std::uint32_t count = 1) noexcept;

  /// @brief Stop timing for good: a submit carrying this timer's queries may
  ///        still run on the device.
  ///
  /// Its queries cannot be reused while it may still write them
  /// (VUID-vkCmdResetQueryPool-None-02841), and nothing says when it drains,
  /// so the pool retires with it: @ref available is false from here on.
  /// Spans already resolved are still published.
  void abandon() noexcept;

  /// @brief Publish each resolved span to @p out as a device row, and end the
  ///        window: for a timer used by hand.
  ///
  /// An unresolved span is skipped, not reported as 0: a missing row means
  /// "not measured", 0.00 "measured, and fast".
  /// @param out  The metrics the rows land in.
  void report_into(StageMetrics& out);

  /// @brief Discard the window's spans without publishing them.
  void reset() noexcept { end_window(); }

  /// @return How many spans the window holds.
  std::size_t count() const noexcept { return spans_.size(); }

  /// @brief Raise the per-window bound to at least @p max_spans, for a caller
  ///        whose window grows with its input.
  ///
  /// Does nothing within a window or on an unavailable timer. A larger pool
  /// that will not allocate is logged, and the timer keeps timing at its
  /// bound. A @p max_spans past @ref kMaxSpans is clamped, logged: a stage
  /// that then fills the window reports no device time rather than part.
  /// @param device     The device the timer was made for.
  /// @param max_spans  The bound wanted.
  /// @return OK; or @ref Status::Code::InvalidArgument for a device other
  ///         than the timer's.
  Status reserve(const Device& device, std::uint32_t max_spans);

  /// @return The query pool, shared, or null: a submit carrying this timer's
  ///         spans keeps it with the rest of its `keep_alive`, so the device
  ///         holds it past a failed wait, and a timer destroyed after
  ///         @ref abandon frees no queries the work may still write.
  std::shared_ptr<void> keep_alive() const noexcept { return pool_; }

 private:
  friend class GpuStageScope;

  struct Span {
    const char* name = nullptr;  // on StageRow::name's terms
    std::uint64_t scope = 0;     // the scope that publishes it; 0 by hand
    double ms = 0.0;
    bool resolved = false;
  };
  struct OpenScope {
    std::uint64_t id = 0;
    bool truncated = false;  // the window refused it a span
  };

  // A GpuStageScope's half. open_scope returns the scope's id; close_scope
  // publishes the spans opened under it into `out` -- none if the window
  // refused it one -- and ends the window when no scope is left open.
  std::uint64_t open_scope();
  void close_scope(std::uint64_t id, StageMetrics& out);
  OpenScope* find_scope(std::uint64_t id) noexcept;

  void clear_state() noexcept;
  // Ends the window, and re-arms the once-a-window warnings for the next.
  void end_window() noexcept;

  VkDevice device_ = VK_NULL_HANDLE;
  // Shared with the submits carrying its spans (keep_alive()).
  std::shared_ptr<QueryPool> pool_;
  std::uint32_t max_spans_ = 0;
  std::uint32_t valid_bits_ = 0;
  float period_ns_ = 0.0F;
  bool warned_full_ = false;
  bool warned_closed_ = false;
  std::vector<Span> spans_;
  std::vector<OpenScope> scopes_;  // innermost last
  std::uint64_t next_scope_ = 1;
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
/// It publishes the spans opened under it and no others, so a helper's scope
/// with metrics of its own, open inside a stage's on one timer, takes none of
/// the stage's spans. A command recorded under it and submitted after it
/// closes runs untimed.
///
/// Null metrics make it inert end to end: @ref timer is null, the work takes
/// the untimed path, and nothing is published.
///
/// @warning The metrics, the timer and the @p name literal must outlive the
///          scope; scopes on one timer nest.
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
  /// @param name     The label; string-literal lifetime, not null.
  GpuStageScope(StageMetrics* metrics, GpuTimer& timer, const char* name)
      : metrics_(metrics), timer_(&timer), host_(metrics, name) {
    if (metrics_ == nullptr) return;
    // Seeded first, so the row precedes any breakdown row the stage adds.
    metrics_->seed(name);
    scope_ = timer_->open_scope();
  }

  /// @brief Publish the device spans, then close the host row: this body runs
  ///        before @ref host_, the last member, is destroyed.
  // As StageScope's destructor: a row's allocation failing terminates.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~GpuStageScope() {
    if (metrics_ != nullptr) timer_->close_scope(scope_, *metrics_);
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
  /// @return What a command recorded now keeps until its submit opens the
  ///         span; its timer is null when inert.
  GpuSpanTag tag() const noexcept {
    if (metrics_ == nullptr) return {};
    return GpuSpanTag{timer_, host_.name(), scope_};
  }

 private:
  StageMetrics* metrics_;
  GpuTimer* timer_;
  std::uint64_t scope_ = 0;
  StageScope host_;
};

}  // namespace volumetric_kit::core
