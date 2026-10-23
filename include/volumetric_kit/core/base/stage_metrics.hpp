// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file stage_metrics.hpp
/// @brief Named host and device spans: the vocabulary every library reports
///        its timings in and every consumer displays.
///
/// Plain data, with no Vulkan in sight, so a host-only consumer -- an
/// exporter, a benchmark, calib's solver -- reads it without a driver header.
/// recon's stage metrics and gfx's frame sections were the same four fields;
/// this is the one type both report in, so an app shows them in one table
/// with no mapping loop.
///
/// A row's host time splits into busy and blocked. Each thread keeps a stack
/// of the scopes open on it, so a blocking wait deep in a library reaches
/// every row whose span contains it (@ref note_blocked), and a device span
/// reaches every row a command was recorded under. The stack holds only
/// scopes with metrics: nothing measured, nothing on it.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/export.hpp"

namespace volumetric_kit::core {

/// @brief What a row's device half holds.
///
/// The states rank in the order listed, and a row accumulating several takes
/// the highest: it reads @ref Measured only when all its device work was
/// measured, so a partial sum never passes for the whole stage.
///
/// @code
/// switch (row.device) {
///   case DeviceTime::None: cell = ""; break;           // host-only stage
///   case DeviceTime::Measured: cell = fmt(row.gpu_ms); break;
///   case DeviceTime::NotTimed: cell = "n/t"; break;
///   case DeviceTime::Unavailable: cell = "n/a"; break;
/// }
/// @endcode
enum class DeviceTime : std::uint8_t {
  /// A host-only stage: no device half.
  None = 0,
  /// @ref StageRow::gpu_ms is the stage's tiled device time.
  Measured = 1,
  /// Its device work ran untimed: no query room, its batch failed, its
  /// timestamps were not yet readable, or it was nested deeper than the
  /// timer tiles (`GpuTimer::set_max_depth`).
  NotTimed = 2,
  /// The queue cannot time (`timestampValidBits` 0), or the timer has no
  /// queries.
  Unavailable = 3,
};

/// @brief One labelled stage: its host span, the part of it spent blocked,
///        and its device span.
///
/// @ref device is what a consumer branches on to show a device column; it
/// says what the device half holds, not what the device can do. A test that
/// must know whether the device can time asks a `GpuTimer` (`available()`).
///
/// The fields added since the first four come last, so a row built
/// positionally (`StageRow{name, cpu_ms, gpu_ms, has_gpu}`) keeps its
/// meaning.
///
/// @code
/// for (const StageRow& row : metrics.rows()) {
///   const double busy = row.cpu_ms - row.wait_ms;
///   print(row.name, busy, row.wait_ms,
///         row.device == DeviceTime::Measured ? row.gpu_ms : -1.0);
/// }
/// @endcode
struct StageRow {
  /// The stage's label, never null. Stored by pointer, not copied, which
  /// keeps this trivially copyable: it must outlive every read of the
  /// metrics -- a string literal, as every caller passes.
  const char* name = nullptr;
  /// Wall-clock host time of the row's scopes, in milliseconds: busy and
  /// blocked together. Around a blocking submit it includes the fence wait,
  /// which @ref wait_ms states.
  double cpu_ms = 0.0;
  /// Device time, in milliseconds; meaningful only when @ref device is
  /// @ref DeviceTime::Measured.
  double gpu_ms = 0.0;
  /// Whether @ref device is @ref DeviceTime::Measured. Deprecated: kept for
  /// source compatibility; read @ref device.
  bool has_gpu = false;
  /// The part of @ref cpu_ms the host spent blocked on the device, in
  /// milliseconds; busy time is `cpu_ms - wait_ms`.
  double wait_ms = 0.0;
  /// What @ref gpu_ms holds.
  DeviceTime device = DeviceTime::None;
};

class StageMetrics;

namespace detail {

/// @brief An open scope's entry on its thread's stack of scopes: what
///        blocking waits and device spans add to until the scope closes and
///        publishes it. The vulkan tier's hook into a scope; callers use
///        @ref StageScope and @ref WaitScope.
struct ScopeFrame {
  /// The frame this one opened inside, on the same thread; null outermost.
  ScopeFrame* outer = nullptr;
  /// The scope's row; string-literal lifetime.
  const char* name = nullptr;
  /// Set by @ref push_scope: unique among the frames ever pushed on this
  /// thread, so a recorded command names its scope without a pointer that a
  /// later scope could reuse.
  std::uint64_t serial = 0;
  /// Blocked time reported inside the scope, in nanoseconds.
  std::int64_t blocked_ns = 0;
  /// Device time credited to the scope, in milliseconds; counts when
  /// @ref device is @ref DeviceTime::Measured.
  double device_ms = 0.0;
  /// What the credited device time holds.
  DeviceTime device = DeviceTime::None;
  /// A @ref WaitScope's frame: its own span is the wait, so a blocking wait
  /// inside it stops here and reaches the outer frames once, when it closes.
  bool wait = false;
};

/// @brief Push @p frame on this thread's stack, as its innermost, and give
///        it its serial.
/// @param frame  The frame; it stays where it is until @ref pop_scope.
VKC_BASE_API void push_scope(ScopeFrame* frame) noexcept;

/// @brief Take @p frame off this thread's stack. A frame that is not on it
///        is left alone.
/// @param frame  A frame @ref push_scope pushed on this thread.
VKC_BASE_API void pop_scope(ScopeFrame* frame) noexcept;

/// @return This thread's innermost open frame, or null when none is open;
///         `outer` walks to the outermost.
VKC_BASE_API ScopeFrame* innermost_scope() noexcept;

}  // namespace detail

/// @brief Report @p ns this thread spent blocked on the device: added to the
///        wait of every scope open on this thread, whose host span already
///        contains it, up to the innermost @ref WaitScope, which covers it.
///
/// What every blocking wait in the core calls -- a blocking submit's fence,
/// `PendingBatch::wait`, `TimelineSemaphore::wait`, `Device::wait_idle` --
/// so a row's @ref StageRow::wait_ms is filled with no change at its call
/// site. With no scope open it records nothing, at the cost of one
/// thread-local read.
///
/// @code
/// const auto start = std::chrono::steady_clock::now();
/// wait_for_my_own_event();  // a wait outside the core
/// note_blocked(std::chrono::duration_cast<std::chrono::nanoseconds>(
///                  std::chrono::steady_clock::now() - start).count());
/// @endcode
/// @param ns  The blocked time, in nanoseconds; 0 or less records nothing.
VKC_BASE_API void note_blocked(std::int64_t ns) noexcept;

/// @brief An accumulating set of named spans, in first-seen order.
///
/// A library takes `StageMetrics* = nullptr` on its entry point and measures
/// nothing when the caller passes null: no state kept between calls, nothing
/// measured when unasked. Repeat spans under one name accumulate, so a stage
/// that runs twice in a frame reports its total. The only state shared
/// between calls is each thread's stack of open scopes, which a null set
/// never joins.
///
/// A name is never null: a null one is a programmer error (`VKC_CHECK`), as
/// every row is matched by its text.
///
/// @code
/// StageMetrics metrics;
/// metrics.seed("texture");             // a row that may not run this frame
/// {
///   StageScope scope(metrics, "integrate");
///   VKC_TRY(integrator.integrate(frame));
/// }                                    // host, wait and device halves land
/// metrics.merge_device(pending.device_rows());  // an async batch's, later
/// @endcode
class StageMetrics {
 public:
  /// @brief The prefix that marks a row as a breakdown of the row above it --
  ///        the phases inside one stage -- rather than a stage of its own.
  ///
  /// The totals skip such rows: the stage above already contains their host
  /// time, their waits and, as device time tiles nest, their device time.
  static constexpr const char* kBreakdownPrefix = "  ..";

  /// @brief Construct an empty set.
  StageMetrics() = default;
  ~StageMetrics() = default;

  /// @brief Copy @p other's rows, not its open scopes: a @ref Scope times the
  ///        set it was opened on, so a copy starts with none.
  /// @param other  The set to copy.
  StageMetrics(const StageMetrics& other) : rows_(other.rows_) {}

  /// @brief Take @p other's rows; its open scopes stay with it.
  /// @param other  The set to take from.
  StageMetrics(StageMetrics&& other) noexcept : rows_(std::move(other.rows_)) {}

  /// @brief Replace the rows with @p other's, keeping this set's open
  ///        scopes, so `metrics = StageMetrics{}` inside a scope clears the
  ///        rows and leaves the scope counted.
  /// @param other  The set to copy.
  /// @return This set.
  StageMetrics& operator=(const StageMetrics& other) {
    if (this != &other) rows_ = other.rows_;
    return *this;
  }

  /// @brief As the copy assignment, taking @p other's rows.
  /// @param other  The set to take from.
  /// @return This set.
  StageMetrics& operator=(StageMetrics&& other) noexcept {
    if (this != &other) rows_ = std::move(other.rows_);
    return *this;
  }

  /// @brief Drop every row, keeping the storage.
  void clear() noexcept { rows_.clear(); }

  /// @brief Add host time to @p name's row, creating it if new.
  /// @param name          The label; string-literal lifetime (see
  ///                      @ref StageRow::name).
  /// @param milliseconds  The span to add.
  void add_cpu(const char* name, double milliseconds) {
    find_or_add(name).cpu_ms += milliseconds;
  }

  /// @brief Add blocked time to @p name's row, creating it if new: the part
  ///        of its host time (@ref add_cpu) spent waiting on the device.
  /// @param name          The label; string-literal lifetime.
  /// @param milliseconds  The blocked time to add.
  void add_wait(const char* name, double milliseconds) {
    find_or_add(name).wait_ms += milliseconds;
  }

  /// @brief Add a device half to @p name's row, creating it if new.
  ///
  /// The row's state becomes the higher of its own and @p device
  /// (@ref DeviceTime), and @p milliseconds counts only when @p device is
  /// @ref DeviceTime::Measured. Separate from @ref add_cpu, as the halves
  /// arrive at different times: a host scope closes when its call returns, a
  /// device span only once the work has completed.
  /// @param name          The label; string-literal lifetime.
  /// @param device        What the half holds; @ref DeviceTime::None adds
  ///                      nothing.
  /// @param milliseconds  The device time, when measured.
  void add_device(const char* name, DeviceTime device,
                  double milliseconds = 0.0) {
    if (device == DeviceTime::None) return;
    StageRow& row = find_or_add(name);
    if (device == DeviceTime::Measured) row.gpu_ms += milliseconds;
    row.device = std::max(row.device, device);
    row.has_gpu = row.device == DeviceTime::Measured;
  }

  /// @brief Add measured device time to @p name's row: @ref add_device with
  ///        @ref DeviceTime::Measured.
  /// @param name          The label; string-literal lifetime.
  /// @param milliseconds  The span to add.
  void add_gpu(const char* name, double milliseconds) {
    add_device(name, DeviceTime::Measured, milliseconds);
  }

  /// @brief Create @p name's row at zero if it does not exist.
  ///
  /// Seeding every stage a frame might run keeps a table's shape stable: a
  /// stage that did not run reports 0.00 rather than dropping its row and
  /// moving every row below it.
  /// @param name  The label; string-literal lifetime.
  void seed(const char* name) { find_or_add(name); }

  /// @brief Fold @p other's rows into this one, matched by name, every half
  ///        of each: host, wait, and device as @ref add_device folds it.
  ///
  /// Rows taken from @p other keep pointing at its labels, which must outlive
  /// every read of this set.
  /// @param other  The set to fold in; merging a set into itself does nothing.
  void merge(const StageMetrics& other) {
    if (this == &other) return;
    for (const StageRow& row : other.rows_) {
      StageRow& dst = find_or_add(row.name);
      dst.cpu_ms += row.cpu_ms;
      dst.wait_ms += row.wait_ms;
      add_device(row.name, device_of(row), row.gpu_ms);
    }
  }

  /// @brief Fold the device halves of @p rows into this set, matched by
  ///        name, leaving every host half as it is: what a caller does with
  ///        an async batch's rows (`PendingBatch::device_rows`) after its
  ///        wait.
  ///
  /// A row of @p rows with no device half adds nothing, not even its row.
  /// @param rows  The rows; their labels must outlive every read of this set.
  void merge_device(const std::vector<StageRow>& rows) {
    for (const StageRow& row : rows) {
      add_device(row.name, device_of(row), row.gpu_ms);
    }
  }

  /// @return The rows, in first-seen order.
  const std::vector<StageRow>& rows() const noexcept { return rows_; }

  /// @return Whether nothing has been recorded.
  bool empty() const noexcept { return rows_.empty(); }

  /// @brief Sum the rows' host time, skipping breakdowns.
  /// @param exclude  A further label to leave out, or null: a row timed for
  ///                 visibility but not part of the reported figure.
  /// @return The sum, in milliseconds.
  double total_cpu_ms(const char* exclude = nullptr) const noexcept {
    double total = 0.0;
    for (const StageRow& row : rows_) {
      if (counted(row, exclude)) total += row.cpu_ms;
    }
    return total;
  }

  /// @brief Sum the rows' blocked time, skipping breakdowns: a wait inside a
  ///        breakdown is in its stage's row too, so it counts once.
  /// @param exclude  As @ref total_cpu_ms.
  /// @return The sum, in milliseconds.
  double total_wait_ms(const char* exclude = nullptr) const noexcept {
    double total = 0.0;
    for (const StageRow& row : rows_) {
      if (counted(row, exclude)) total += row.wait_ms;
    }
    return total;
  }

  /// @brief Sum the measured rows' device time, skipping breakdowns, whose
  ///        tiles their stage's time already contains.
  /// @param exclude  As @ref total_cpu_ms.
  /// @return The sum, in milliseconds; a row not @ref DeviceTime::Measured
  ///         adds nothing.
  double total_gpu_ms(const char* exclude = nullptr) const noexcept {
    double total = 0.0;
    for (const StageRow& row : rows_) {
      if (counted(row, exclude) && row.device == DeviceTime::Measured) {
        total += row.gpu_ms;
      }
    }
    return total;
  }

  /// @return Whether a @ref Scope is open on this set: a row added now is
  ///         nested in a stage whose host span contains it, so a callee
  ///         reached from both positions can name its row a breakdown or a
  ///         stage of its own.
  bool in_stage() const noexcept { return open_scopes_ > 0; }

  class Scope;

 private:
  friend class WaitScope;

  // Matched by content, not pointer: one stage seeded in one translation unit
  // and timed in another names two literals, which need not share an address.
  StageRow& find_or_add(const char* name) {
    VKC_CHECK(name != nullptr, "StageMetrics: a row's name is null");
    for (StageRow& row : rows_) {
      if (std::strcmp(row.name, name) == 0) return row;
    }
    rows_.push_back(StageRow{name, 0.0, 0.0, false, 0.0, DeviceTime::None});
    return rows_.back();
  }

  // A row built before `device` existed says Measured through has_gpu alone.
  static DeviceTime device_of(const StageRow& row) noexcept {
    if (row.device == DeviceTime::None && row.has_gpu) {
      return DeviceTime::Measured;
    }
    return row.device;
  }

  // Whether a total counts `row`: not a breakdown, and not `exclude`.
  static bool counted(const StageRow& row, const char* exclude) noexcept {
    if (exclude != nullptr && std::strcmp(row.name, exclude) == 0) {
      return false;
    }
    return std::strncmp(row.name, kBreakdownPrefix,
                        std::strlen(kBreakdownPrefix)) != 0;
  }

  std::vector<StageRow> rows_;
  // The scopes open on this object, not part of its value: neither copied
  // nor assigned, so a set reset by assignment inside a scope stays in it.
  int open_scopes_ = 0;
};

/// @brief Times its own scope into a @ref StageMetrics row -- host time, the
///        blocking waits inside it, and the device time of the commands
///        recorded under it -- and is inert on null metrics, so a library's
///        `StageMetrics* = nullptr` needs no `if`.
///
/// It joins its thread's stack of open scopes while open, which is how a
/// wait (@ref note_blocked) or a device span reaches it.
///
/// @warning The metrics and the @p name literal must outlive the scope, and
///          the scope opens and closes on one thread.
///
/// @code
/// Status Integrator::integrate(const Frame& frame, StageMetrics* metrics) {
///   StageScope scope(metrics, "integrate");  // inert when null
///   ...
/// }
/// @endcode
class StageMetrics::Scope {
 public:
  /// @brief Start timing @p name into @p metrics.
  /// @param metrics  The set the span lands in.
  /// @param name     The label; string-literal lifetime, not null.
  Scope(StageMetrics& metrics, const char* name) : Scope(&metrics, name) {}

  /// @brief Start timing @p name into @p metrics, or nothing when it is null.
  /// @param metrics  The set the span lands in, or null.
  /// @param name     The label; string-literal lifetime, and not null when
  ///                 @p metrics is not (`VKC_CHECK`).
  Scope(StageMetrics* metrics, const char* name)
      : metrics_(metrics), start_(Clock::now()) {
    frame_.name = name;
    if (metrics_ == nullptr) return;
    // Here, not where the row is added as the scope closes, so the abort
    // names the call that passed it.
    VKC_CHECK(name != nullptr, "StageScope: the name is null");
    ++metrics_->open_scopes_;
    detail::push_scope(&frame_);
  }

  /// @brief Stop timing and add the span, its waits and its device time,
  ///        unless inert.
  // Adding a row may allocate; out of memory here terminates, as from any
  // destructor -- for a diagnostic, the same end the allocation failure would
  // reach anywhere else.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~Scope() {
    if (metrics_ == nullptr) return;
    // Closed before the row is added, so whatever runs next sees the nesting
    // it is really in.
    detail::pop_scope(&frame_);
    --metrics_->open_scopes_;
    metrics_->add_cpu(frame_.name, std::chrono::duration<double, std::milli>(
                                       Clock::now() - start_)
                                       .count());
    if (frame_.blocked_ns > 0) {
      metrics_->add_wait(frame_.name,
                         static_cast<double>(frame_.blocked_ns) * 1e-6);
    }
    metrics_->add_device(frame_.name, frame_.device, frame_.device_ms);
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;

  /// @return The row this times into.
  const char* name() const noexcept { return frame_.name; }

  /// @return This scope's serial on its thread's stack, which a recorded
  ///         command keeps to find it again; 0 when inert.
  std::uint64_t serial() const noexcept { return frame_.serial; }

 private:
  using Clock = std::chrono::steady_clock;

  StageMetrics* metrics_;
  detail::ScopeFrame frame_;
  Clock::time_point start_;
};

/// @brief The name every library times its stages with.
using StageScope = StageMetrics::Scope;

/// @brief Times a span the host spends blocked into a row of its own, whose
///        host time is all wait (`cpu_ms == wait_ms`); inert on null metrics.
///
/// For a wait the core does not see -- pacing, a condition variable -- or
/// one worth its own row, such as waiting for a pending batch. A blocking
/// wait inside it is its own span, not reported again; once it closes, its
/// whole span reaches the scopes open around it as blocked time
/// (@ref note_blocked).
///
/// @warning The metrics and the @p name literal must outlive the scope, and
///          the scope opens and closes on one thread.
///
/// @code
/// if (!pending.ready()) {
///   const WaitScope wait(&metrics, "wait: GPU behind");
///   VKC_TRY(pending.wait());
/// }
/// @endcode
class WaitScope {
 public:
  /// @brief Start timing a wait named @p name into @p metrics, or nothing
  ///        when it is null.
  /// @param metrics  The set the span lands in, or null.
  /// @param name     The label; string-literal lifetime, and not null when
  ///                 @p metrics is not (`VKC_CHECK`).
  WaitScope(StageMetrics* metrics, const char* name)
      : metrics_(metrics), start_(Clock::now()) {
    frame_.name = name;
    frame_.wait = true;
    if (metrics_ == nullptr) return;
    VKC_CHECK(name != nullptr, "WaitScope: the name is null");
    ++metrics_->open_scopes_;
    detail::push_scope(&frame_);
  }

  /// @brief Stop timing; add the span as host and wait time, and report it
  ///        to the scopes open around this one.
  // As StageScope's destructor: a row's allocation failing terminates.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~WaitScope() {
    if (metrics_ == nullptr) return;
    detail::pop_scope(&frame_);
    --metrics_->open_scopes_;
    const std::int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             start_)
            .count();
    const double ms = static_cast<double>(ns) * 1e-6;
    metrics_->add_cpu(frame_.name, ms);
    metrics_->add_wait(frame_.name, ms);
    note_blocked(ns);
  }

  WaitScope(const WaitScope&) = delete;
  WaitScope& operator=(const WaitScope&) = delete;
  WaitScope(WaitScope&&) = delete;
  WaitScope& operator=(WaitScope&&) = delete;

  /// @return The row this times into.
  const char* name() const noexcept { return frame_.name; }

 private:
  using Clock = std::chrono::steady_clock;

  StageMetrics* metrics_;
  detail::ScopeFrame frame_;
  Clock::time_point start_;
};

}  // namespace volumetric_kit::core
