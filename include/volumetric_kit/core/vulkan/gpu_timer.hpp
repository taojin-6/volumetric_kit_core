// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file gpu_timer.hpp
/// @brief Device time, tiled: timestamps at the boundaries between a command
///        buffer's stages, so a stage's time is everything the device did
///        for it -- its dispatches, fills, copies and barriers -- and the
///        stages of one command buffer add up to its span.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/export.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

class CommandBatch;
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

/// @brief What a recorded command keeps of a @ref GpuStageScope: the timer,
///        the label, and the scope by its serial on its thread
///        (@ref StageScope::serial).
///
/// A value, so a scope that closes before the submit leaves nothing to
/// dangle.
///
/// @code
/// const GpuSpanTag tag = stage.tag();  // when the command is recorded
/// if (tag.timer == nullptr) { /* untimed: null metrics */ }
/// @endcode
struct GpuSpanTag {
  /// The timer a batch with no timer of its own times in; null is untimed.
  GpuTimer* timer = nullptr;
  /// The label, on @ref StageRow::name's terms.
  const char* name = nullptr;
  /// The scope's serial, or 0 for none.
  std::uint64_t scope = 0;
};

/// @brief A command buffer's device span, from its first timestamp to its
///        last.
///
/// Raw ticks always: two spans on one queue compare in them directly -- the
/// device's idle time between two batches -- with no calibration. On the
/// host's clock too where the device offers calibrated timestamps
/// (@ref Device::calibrate), so a span lines up with host stamps: the queue
/// latency from a submit call to the first tile.
///
/// @code
/// if (const std::optional<DeviceSpan> span = pending.device_span()) {
///   if (span->host_clock) queue_ns = span->begin_ns - submit_call_ns;
/// }
/// @endcode
struct DeviceSpan {
  /// The first timestamp, in the queue's ticks (`timestampValidBits` wide).
  std::uint64_t begin_ticks = 0;
  /// The last timestamp, in the same ticks.
  std::uint64_t end_ticks = 0;
  /// The first timestamp on `std::chrono::steady_clock`, in nanoseconds
  /// since its epoch; meaningful when @ref host_clock.
  std::int64_t begin_ns = 0;
  /// The last timestamp, likewise.
  std::int64_t end_ns = 0;
  /// How far @ref begin_ns and @ref end_ns may be off, in nanoseconds: the
  /// calibration's uncertainty.
  std::uint64_t deviation_ns = 0;
  /// Whether @ref begin_ns and @ref end_ns hold host times.
  bool host_clock = false;
};

namespace detail {

/// @brief The tile a tagged command counts to: the scope it was tagged
///        with, and every scope open around that one on the recording thread
///        when it was recorded, outermost first. A command's time counts to
///        each of them, as its host time does. @ref CommandBatch keeps one
///        per scope it records under.
struct TileKey {
  /// The scopes' rows, outermost first; the tag's own last.
  std::vector<const char*> names;
  /// Their serials (@ref StageScope::serial), in the same order; 0 where a
  /// scope was not open on the recording thread.
  std::vector<std::uint64_t> serials;
};

/// @brief The key a command recorded now under @p tag counts to: @p tag's
///        scope and the scopes open around it on this thread, or the tag's
///        name alone when its scope is not open here.
/// @param tag  A command's tag, its timer not null.
/// @return The key.
VKC_VULKAN_API TileKey tile_key(const GpuSpanTag& tag);

}  // namespace detail

/// @brief The query pool device time is measured on, shared by every batch
///        it times: each takes a range of it from recording until its results
///        are read.
///
/// A batch holds its range while it is pending, so a timer times any number
/// of batches in flight, each read after its own work. Two timestamps are
/// written per tile boundary at most -- one each, plus one at each end -- so
/// a batch of `n` tiles needs `n + 1` queries.
///
/// **Unavailable is not an error.** A queue family may have no timestamps
/// (`timestampValidBits` 0, as some implementations and some compute-only
/// families report), and a pool may fail to allocate. @ref create still
/// succeeds and @ref available is false: what it would time reads
/// @ref DeviceTime::Unavailable, so a caller writes the same code either
/// way. A diagnostic must not be able to fail the work it measures;
/// @ref create refuses only a caller's mistake. A batch that finds no free
/// range runs untimed, its rows @ref DeviceTime::NotTimed.
///
/// @warning The device passed to @ref create must outlive the timer.
/// @warning Not synchronized: one thread records with it at a time. A
///          pending batch's wait, on any thread, returns its range safely.
///
/// @code
/// VKC_ASSIGN(GpuTimer timer, GpuTimer::create(device));  // once
/// for (;;) {
///   StageMetrics metrics;
///   {
///     GpuStageScope stage(&metrics, timer, "integrate");
///     CommandBatch batch(device, allocator);
///     VKC_TRY(batch.dispatch(kernel, &push, sizeof(push), groups, max,
///                            &stage));
///     VKC_TRY(batch.submit());
///   }  // host, wait and device halves land in metrics
/// }
/// @endcode
class VKC_VULKAN_API GpuTimer {
 public:
  /// The ceiling on @ref create's `max_spans`: 4096 queries, two a span.
  /// An implementation layered on Metal backs a timestamp pool with a counter
  /// sample buffer of at most 32 KiB -- 4096 timestamps -- and silently
  /// emulates timing for a larger one.
  static constexpr std::uint32_t kMaxSpans = 2048U;

  /// @brief Construct an empty timer; @ref valid is false.
  GpuTimer() noexcept = default;

  /// @brief Create a timer for @p device's queue family.
  /// @param device     Supplies the family's timestamp bits and period, and
  ///                   its calibrated clocks; it must outlive the timer.
  /// @param max_spans  The pool's size, in pairs of queries: `2 * max_spans`
  ///                   timestamps, shared by the batches pending at once; in
  ///                   [1, @ref kMaxSpans].
  /// @return The timer -- with @ref available false, and the reason logged,
  ///         where the family has no timestamps or the pool will not
  ///         allocate; or @ref Status::Code::InvalidArgument for an empty
  ///         @p device or a @p max_spans out of range.
  static Result<GpuTimer> create(const Device& device,
                                 std::uint32_t max_spans = 32);

  ~GpuTimer();
  GpuTimer(GpuTimer&& other) noexcept;
  GpuTimer& operator=(GpuTimer&& other) noexcept;
  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  /// @return Whether the timer can time; when false, what it would time
  ///         reads @ref DeviceTime::Unavailable.
  bool available() const noexcept;
  /// @return Whether this owns a timer (false when moved-from). A timer on a
  ///         device without timestamps is still valid, just not available.
  bool valid() const noexcept { return device_ != VK_NULL_HANDLE; }

  /// @brief Tile only scopes nested at most @p depth deep, counted from the
  ///        outermost scope open on the recording thread: a command tagged
  ///        deeper counts to its ancestor at that depth, and the deeper
  ///        scopes read @ref DeviceTime::NotTimed.
  ///
  /// 1 gives stage totals with the fewest timestamps -- on a device where a
  /// timestamp may cost an encoder boundary, the least perturbation. Read by
  /// a batch when it submits.
  /// @param depth  The depth; 0, the default, tiles every scope.
  void set_max_depth(std::uint32_t depth) noexcept { max_depth_ = depth; }
  /// @return The depth @ref set_max_depth set; 0 for every scope.
  std::uint32_t max_depth() const noexcept { return max_depth_; }

  /// @brief Grow the pool to at least @p max_spans pairs of queries, for a
  ///        caller that times more at once.
  ///
  /// A pending batch keeps the pool it took its range from, so this is safe
  /// at any time. Does nothing on an unavailable timer or for a size it
  /// already has. A larger pool that will not allocate is logged, and the
  /// timer keeps its own. A @p max_spans past @ref kMaxSpans is clamped,
  /// logged.
  /// @param device     The device the timer was made for.
  /// @param max_spans  The size wanted.
  /// @return OK; or @ref Status::Code::InvalidArgument for a device other
  ///         than the timer's.
  Status reserve(const Device& device, std::uint32_t max_spans);

  /// @return The query pool, shared, or null: a submit timed on this timer
  ///         keeps it with the rest of its `keep_alive`, so the device holds
  ///         it past a failed wait, and a timer destroyed meanwhile frees no
  ///         queries the work may still write.
  std::shared_ptr<void> keep_alive() const noexcept;

 private:
  friend class GpuTiles;
  struct Queries;  // the pool, its ranges and its clock, shared with batches

  VkDevice device_ = VK_NULL_HANDLE;
  std::shared_ptr<Queries> queries_;
  std::uint32_t max_depth_ = 0;
};

/// @brief One command buffer's tiles: a range of a @ref GpuTimer's queries,
///        held from recording until the work's timestamps are read.
///
/// A timestamp (`BOTTOM_OF_PIPE`) is written before the first command, at
/// each change of tile, and after the last command, so everything between
/// two marks -- dispatches, fills, copies, barriers, gaps -- counts to the
/// tile they bound, and the tiles add up to the span. The first named tile
/// covers what was recorded before it. A mark is written once earlier work
/// on the queue completes, so the tiles of successive command buffers on a
/// queue never overlap; a device may latch it later than the boundary, and
/// one layered on Metal may snap it to an encoder boundary.
///
/// What @ref CommandBatch times with. By hand, for a command buffer the
/// caller submits itself -- a renderer's frame -- outside a render pass:
///
/// @code
/// GpuTiles tiles = GpuTiles::begin(timer, cmd, 2);  // reset + first mark
/// tiles.mark(cmd, "shadow pass");                   // names the open tile
/// record_shadow_pass(cmd);
/// tiles.mark(cmd, "main pass");                     // a mark: a new tile
/// record_main_pass(cmd);
/// tiles.end(cmd);                                   // the last mark
/// // ... submit cmd; once its fence has signalled:
/// tiles.resolve();
/// metrics.merge_device(tiles.rows());
/// @endcode
///
/// @warning Move-only. Destroying one, or moving over it, returns its range:
///          its command buffer must have completed or never run by then --
///          @ref drop one the device may still run.
class VKC_VULKAN_API GpuTiles {
 public:
  /// @brief Construct untimed tiles: no range, no rows.
  GpuTiles() noexcept = default;
  ~GpuTiles();
  GpuTiles(GpuTiles&& other) noexcept;
  GpuTiles& operator=(GpuTiles&& other) noexcept;
  GpuTiles(const GpuTiles&) = delete;
  GpuTiles& operator=(const GpuTiles&) = delete;

  /// @brief Take a range for up to @p tiles tiles from @p timer, and record
  ///        its reset and the first mark into @p cmd.
  /// @param timer  The timer whose queries the marks go to.
  /// @param cmd    A recording command buffer, outside a render pass.
  /// @param tiles  The most tiles; 0 counts as 1.
  /// @return The tiles; untimed when @p timer is unavailable, or has no free
  ///         range, whose rows read @ref DeviceTime::Unavailable or
  ///         @ref DeviceTime::NotTimed.
  static GpuTiles begin(GpuTimer& timer, VkCommandBuffer cmd,
                        std::uint32_t tiles);

  /// @brief Start a tile named @p name: write a mark into @p cmd, unless no
  ///        tile is named yet -- the first name names the open tile -- or
  ///        @p name is the open tile's.
  ///
  /// A tile past the room @ref begin took is not marked: every tile then
  /// reads @ref DeviceTime::NotTimed, and the span stays.
  /// @param cmd   The command buffer @ref begin recorded into.
  /// @param name  The tile's row, on @ref StageRow::name's terms; not null
  ///              (`VKC_CHECK`).
  void mark(VkCommandBuffer cmd, const char* name);

  /// @brief Write the last mark into @p cmd, after everything it records.
  /// @param cmd  The command buffer @ref begin recorded into.
  void end(VkCommandBuffer cmd);

  /// @brief Read the marks and return the range, once the command buffer has
  ///        completed. Never waits: a mark not yet readable leaves the tiles
  ///        it bounds @ref DeviceTime::NotTimed.
  /// @pre The command buffer has completed (its fence or timeline value has
  ///      been reached).
  void resolve();

  /// @brief Give up on the timestamps: the command buffer failed. Every tile
  ///        reads @ref DeviceTime::NotTimed; the range goes back, unless the
  ///        device may still run the work, when it is never reused.
  /// @param may_still_run  Whether the device may still run it: after a
  ///                       failed wait, or a submit that lost the device.
  void drop(bool may_still_run);

  /// @return The tiles' rows by name, once resolved or dropped: each tile's,
  ///         added up, with only a device half; empty before, or for tiles
  ///         that named none.
  const std::vector<StageRow>& rows() const noexcept { return rows_.rows(); }

  /// @return The span from the first mark to the last, once resolved, where
  ///         both were readable; empty otherwise.
  std::optional<DeviceSpan> span() const noexcept { return span_; }

  /// @return Whether a range was taken, so marks are written.
  bool timed() const noexcept { return capacity_ != 0; }

 private:
  friend class CommandBatch;
  friend class Device;

  static constexpr std::uint32_t kNoKey = 0xFFFFFFFFU;

  // CommandBatch's and Device's form: keys made at record time, marked by
  // index, each tiled at the timer's max depth; publish() credits the
  // scopes still open on this thread.
  static GpuTiles open(GpuTimer& timer, VkCommandBuffer cmd,
                       std::uint32_t tiles, std::vector<detail::TileKey> keys);
  void mark_key(VkCommandBuffer cmd, std::uint32_t key);
  void publish() const noexcept;
  // Whether keys a and b of `keys` are one tile: equal down to `depth`
  // (0: every scope).
  static bool same_tile(const std::vector<detail::TileKey>& keys,
                        std::uint32_t depth, std::uint32_t a,
                        std::uint32_t b) noexcept;
  // Calls fn(name, serial, state, ms) for each scope each tile counts to,
  // and for each deeper scope, NotTimed.
  template <class Fn>
  void for_each_credit(const Fn& fn) const;
  // Fills tile_ms_, tile_state_ and rows_; `marks` is null when nothing was
  // read.
  void settle(const std::uint64_t* marks);
  void release() noexcept;

  std::shared_ptr<GpuTimer::Queries> queries_;
  std::uint32_t first_ = 0;     // the range's first query
  std::uint32_t capacity_ = 0;  // its queries; 0 untimed
  std::uint32_t written_ = 0;   // marks written
  std::uint32_t depth_ = 0;     // the timer's max depth; 0 every scope
  DeviceTime untimed_ = DeviceTime::None;  // why untimed, for the rows
  bool overflowed_ = false;                // a tile past the room
  bool ended_ = false;                     // the last mark is recorded
  bool leaked_ = false;                    // the range is never reused
  bool settled_ = false;                   // resolved or dropped
  std::vector<detail::TileKey> keys_;
  std::vector<std::uint32_t> tiles_;  // each tile's key, kNoKey unnamed
  // Each tile's time and state once settled, for publish().
  std::vector<double> tile_ms_;
  std::vector<DeviceTime> tile_state_;
  StageMetrics rows_;
  std::optional<DeviceSpan> span_;
};

/// @brief One stage's scope: times its host span, tags the commands recorded
///        under it, and publishes its host, wait and device time when it
///        closes.
///
/// A @ref StageScope whose commands are tagged: a command given it counts to
/// it and to every scope open around it when recorded (@ref GpuTiles). Its
/// device time lands at its close when a blocking submit has run the work
/// by then; a pending batch hands it back by name instead
/// (`PendingBatch::device_rows`), as nothing points at the caller's metrics
/// while the batch is pending.
///
/// Null metrics make it inert end to end: @ref timer is null, the work takes
/// the untimed path, and nothing is published.
///
/// @warning The metrics, the timer and the @p name literal must outlive the
///          scope, which opens and closes on one thread.
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
  /// @brief Open @p name's scope on @p metrics, its commands timed in
  ///        @p timer; inert when @p metrics is null.
  /// @param metrics  The set every half lands in, or null.
  /// @param timer    The timer a batch with none of its own times in.
  /// @param name     The label; string-literal lifetime, not null.
  GpuStageScope(StageMetrics* metrics, GpuTimer& timer, const char* name)
      : timer_(metrics != nullptr ? &timer : nullptr), host_(metrics, name) {
    // Seeded now, so the row precedes any breakdown row the stage adds.
    if (metrics != nullptr) metrics->seed(name);
  }

  ~GpuStageScope() = default;
  GpuStageScope(const GpuStageScope&) = delete;
  GpuStageScope& operator=(const GpuStageScope&) = delete;
  GpuStageScope(GpuStageScope&&) = delete;
  GpuStageScope& operator=(GpuStageScope&&) = delete;

  /// @return The timer to record into, or null when inert -- the untimed
  ///         path.
  GpuTimer* timer() const noexcept { return timer_; }
  /// @return The label device time publishes under.
  const char* name() const noexcept { return host_.name(); }
  /// @return What a command recorded now keeps; its timer is null when
  ///         inert.
  GpuSpanTag tag() const noexcept {
    if (timer_ == nullptr) return {};
    return GpuSpanTag{timer_, host_.name(), host_.serial()};
  }

 private:
  GpuTimer* timer_;
  StageScope host_;
};

}  // namespace volumetric_kit::core
