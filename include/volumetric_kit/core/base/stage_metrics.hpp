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

#include <chrono>
#include <cstring>
#include <vector>

namespace volumetric_kit::core {

/// @brief One labelled stage: its host span and, where a device timer ran,
///        its GPU span.
///
/// @ref has_gpu is what a consumer branches on to show a device column. It
/// means "a device span was measured for this row", not "the device can time":
/// it is false for a host-only stage, for a call that returned before
/// dispatching anything (whose host row is still charged, as a stage silent on
/// failure reads as one that did not run), and for every stage on a queue
/// family without timestamps. A test that must know whether the device can
/// time asks a `GpuTimer` (`available()`), not this.
///
/// @code
/// for (const StageRow& row : metrics.rows()) {
///   print(row.name, row.cpu_ms, row.has_gpu ? row.gpu_ms : -1.0);
/// }
/// @endcode
struct StageRow {
  /// The stage's label. Stored by pointer, not copied, which keeps this
  /// trivially copyable: it must outlive every read of the metrics -- a
  /// string literal, as every caller passes.
  const char* name = nullptr;
  /// Wall-clock host time, in milliseconds. Around a blocking submit this
  /// covers recording, submission, the fence wait and the device's work
  /// together -- an end-to-end cost, which @ref gpu_ms separates.
  double cpu_ms = 0.0;
  /// Device time, in milliseconds; meaningful only when @ref has_gpu is set.
  double gpu_ms = 0.0;
  /// Whether @ref gpu_ms holds a measurement.
  bool has_gpu = false;
};

/// @brief An accumulating set of named spans, in first-seen order.
///
/// A library takes `StageMetrics* = nullptr` on its entry point and measures
/// nothing when the caller passes null: no global sink, no state kept between
/// calls, nothing measured when unasked. Repeat spans under one name
/// accumulate, so a stage that runs twice in a frame reports its total.
///
/// @code
/// StageMetrics metrics;
/// metrics.seed("texture");             // a row that may not run this frame
/// {
///   StageScope scope(metrics, "integrate");
///   VKC_TRY(integrator.integrate(frame));
/// }                                    // the host span lands here
/// timer.report_into(metrics);          // device spans, where a timer ran
/// @endcode
class StageMetrics {
 public:
  /// @brief The prefix that marks a row as a breakdown of the row above it --
  ///        the phases inside one stage -- rather than a stage of its own.
  ///
  /// @ref total_cpu_ms skips such rows, as the stage above already contains
  /// their host time. @ref total_gpu_ms keeps them: a device span covers one
  /// dispatch, so a breakdown row's GPU half is a separate dispatch no row
  /// above contains.
  static constexpr const char* kBreakdownPrefix = "  ..";

  /// @brief Drop every row, keeping the storage.
  void clear() noexcept { rows_.clear(); }

  /// @brief Add host time to @p name's row, creating it if new.
  /// @param name          The label; string-literal lifetime (see
  ///                      @ref StageRow::name).
  /// @param milliseconds  The span to add.
  void add_cpu(const char* name, double milliseconds) {
    find_or_add(name).cpu_ms += milliseconds;
  }

  /// @brief Add device time to @p name's row, creating it if new, and mark the
  ///        row measured.
  ///
  /// Separate from @ref add_cpu, as the halves arrive at different times: a
  /// host scope closes when its call returns, a device span only once the
  /// submit's fence has signalled.
  /// @param name          The label; string-literal lifetime.
  /// @param milliseconds  The span to add.
  void add_gpu(const char* name, double milliseconds) {
    StageRow& row = find_or_add(name);
    row.gpu_ms += milliseconds;
    row.has_gpu = true;
  }

  /// @brief Create @p name's row at zero if it does not exist.
  ///
  /// Seeding every stage a frame might run keeps a table's shape stable: a
  /// stage that did not run reports 0.00 rather than dropping its row and
  /// moving every row below it.
  /// @param name  The label; string-literal lifetime.
  void seed(const char* name) { find_or_add(name); }

  /// @brief Fold @p other's rows into this one, matched by name, both halves
  ///        of each.
  ///
  /// Rows taken from @p other keep pointing at its labels, which must outlive
  /// every read of this set.
  /// @param other  The set to fold in; merging a set into itself does nothing.
  void merge(const StageMetrics& other) {
    if (this == &other) return;
    for (const StageRow& row : other.rows_) {
      StageRow& dst = find_or_add(row.name);
      dst.cpu_ms += row.cpu_ms;
      if (row.has_gpu) {
        dst.gpu_ms += row.gpu_ms;
        dst.has_gpu = true;
      }
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
      if (excluded(row, exclude)) continue;
      if (std::strncmp(row.name, kBreakdownPrefix,
                       std::strlen(kBreakdownPrefix)) == 0) {
        continue;
      }
      total += row.cpu_ms;
    }
    return total;
  }

  /// @brief Sum the rows' device time, breakdowns included (see
  ///        @ref kBreakdownPrefix); rows without a measurement add nothing.
  /// @param exclude  As @ref total_cpu_ms.
  /// @return The sum, in milliseconds.
  double total_gpu_ms(const char* exclude = nullptr) const noexcept {
    double total = 0.0;
    for (const StageRow& row : rows_) {
      if (row.has_gpu && !excluded(row, exclude)) total += row.gpu_ms;
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
  // Matched by content, not pointer: one stage seeded in one translation unit
  // and timed in another names two literals, which need not share an address.
  StageRow& find_or_add(const char* name) {
    for (StageRow& row : rows_) {
      if (std::strcmp(row.name, name) == 0) return row;
    }
    rows_.push_back(StageRow{name, 0.0, 0.0, false});
    return rows_.back();
  }

  static bool excluded(const StageRow& row, const char* exclude) noexcept {
    return exclude != nullptr && std::strcmp(row.name, exclude) == 0;
  }

  std::vector<StageRow> rows_;
  int open_scopes_ = 0;
};

/// @brief Times its own scope into a @ref StageMetrics row; inert on null
///        metrics, so a library's `StageMetrics* = nullptr` needs no `if`.
///
/// @warning The metrics and the @p name literal must outlive the scope.
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
  /// @param name     The label; string-literal lifetime.
  Scope(StageMetrics& metrics, const char* name) : Scope(&metrics, name) {}

  /// @brief Start timing @p name into @p metrics, or nothing when it is null.
  /// @param metrics  The set the span lands in, or null.
  /// @param name     The label; string-literal lifetime.
  Scope(StageMetrics* metrics, const char* name)
      : metrics_(metrics), name_(name), start_(Clock::now()) {
    if (metrics_ != nullptr) ++metrics_->open_scopes_;
  }

  /// @brief Stop timing and add the span, unless inert.
  // Adding a row may allocate; out of memory here terminates, as from any
  // destructor (and aborts under -fno-exceptions) -- for a diagnostic, the
  // same end the allocation failure would reach anywhere else.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~Scope() {
    if (metrics_ == nullptr) return;
    // Closed before the row is added, so whatever runs next sees the nesting
    // it is really in.
    --metrics_->open_scopes_;
    metrics_->add_cpu(
        name_, std::chrono::duration<double, std::milli>(Clock::now() - start_)
                   .count());
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;

  /// @return The row this times into.
  const char* name() const noexcept { return name_; }

 private:
  using Clock = std::chrono::steady_clock;

  StageMetrics* metrics_;
  const char* name_;
  Clock::time_point start_;
};

/// @brief The name every library times its stages with.
using StageScope = StageMetrics::Scope;

}  // namespace volumetric_kit::core
