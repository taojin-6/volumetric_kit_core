// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/gpu_timer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "device_access.hpp"
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
// How long a reading of the clocks serves: the tick period a driver reports
// may drift from the counter's, and a second keeps the error small at one
// read a second.
constexpr std::chrono::seconds kCalibrationLife{1};

// `to - from` in ticks of a `valid_bits` counter, negative when `to` is the
// earlier: a span converts against a pair read before or after it.
std::int64_t signed_ticks(std::uint64_t from, std::uint64_t to,
                          std::uint32_t valid_bits) noexcept {
  if (valid_bits == 0) return 0;
  const std::uint64_t delta = timestamp_delta(from, to, valid_bits);
  if (valid_bits >= 64) return static_cast<std::int64_t>(delta);
  const std::uint64_t half = std::uint64_t{1} << (valid_bits - 1);
  return delta >= half ? static_cast<std::int64_t>(delta) -
                             static_cast<std::int64_t>(half << 1)
                       : static_cast<std::int64_t>(delta);
}

}  // namespace

// The pool and what the batches timed on it share: each holds its range
// from recording until its results are read, which a pending batch's wait
// does on any thread, so the ranges and the clocks are under one mutex.
struct GpuTimer::Queries {
  struct Range {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
  };

  QueryPool pool;
  std::uint32_t valid_bits = 0;
  float period_ns = 0.0F;
  detail::Calibrator calibrator;

  std::mutex mutex;
  std::vector<Range> held;  // by first
  bool warned_full = false;
  bool warned_clock = false;
  std::optional<ClockPair> clock;
  std::chrono::steady_clock::time_point clock_read;

  // The first free run of `count` queries, now held; none when no run fits.
  std::optional<std::uint32_t> acquire(std::uint32_t count) {
    const std::scoped_lock lock(mutex);
    const std::uint32_t size = pool.query_count();
    std::uint32_t at = 0;
    for (auto it = held.begin();; ++it) {
      const std::uint32_t limit = it == held.end() ? size : it->first;
      if (limit >= at && limit - at >= count) {
        held.insert(it, Range{at, count});
        return at;
      }
      if (it == held.end()) break;
      at = it->first + it->count;
    }
    if (!warned_full) {
      warned_full = true;
      log_message(LogLevel::Warning, kLogSource,
                  "GpuTimer: no free run of " + std::to_string(count) +
                      " queries; the batch runs untimed (NotTimed). "
                      "GpuTimer::reserve makes room");
    }
    return std::nullopt;
  }

  void release(std::uint32_t first) noexcept {
    const std::scoped_lock lock(mutex);
    const auto it = std::find_if(
        held.begin(), held.end(),
        [first](const Range& range) { return range.first == first; });
    if (it != held.end()) held.erase(it);
  }

  // A reading of the clocks at most a second old; none where the device has
  // no calibrated timestamps, or they fail (logged once).
  std::optional<ClockPair> clock_pair() {
    if (calibrator.get == nullptr) return std::nullopt;
    const std::scoped_lock lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!clock.has_value() || now - clock_read >= kCalibrationLife) {
      // An expired pair cannot put new spans on the host clock if this read
      // fails, even after the once-only warning has already been emitted.
      clock.reset();
      Result<ClockPair> read = detail::calibrate(calibrator);
      if (read.ok()) {
        clock = *read;
        clock_read = now;
      } else if (!warned_clock) {
        warned_clock = true;
        log_message(LogLevel::Warning, kLogSource,
                    "GpuTimer: the clocks were not read (" +
                        read.status().message() +
                        "); device spans stay in ticks");
      }
    }
    return clock;
  }
};

namespace detail {

TileKey tile_key(const GpuSpanTag& tag) {
  VKC_CHECK(tag.name != nullptr, "GpuTimer: a tag's name is null");
  TileKey key;
  const ScopeFrame* frame = innermost_scope();
  while (frame != nullptr && (tag.scope == 0 || frame->serial != tag.scope)) {
    frame = frame->outer;
  }
  if (frame == nullptr) {
    key.names.push_back(tag.name);
    key.serials.push_back(0);
    return key;
  }
  for (; frame != nullptr; frame = frame->outer) {
    if (frame->wait) continue;  // a wait's row has no device half
    key.names.push_back(frame->name);
    key.serials.push_back(frame->serial);
  }
  std::reverse(key.names.begin(), key.names.end());
  std::reverse(key.serials.begin(), key.serials.end());
  return key;
}

}  // namespace detail

GpuTimer::~GpuTimer() = default;

GpuTimer::GpuTimer(GpuTimer&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      queries_(std::move(other.queries_)),
      max_depth_(std::exchange(other.max_depth_, 0)) {}

GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
  if (this != &other) {
    device_ = std::exchange(other.device_, VK_NULL_HANDLE);
    // Drops this timer's pool, unless a batch still holds it.
    queries_ = std::move(other.queries_);
    max_depth_ = std::exchange(other.max_depth_, 0);
  }
  return *this;
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
  timer.queries_ = std::make_shared<Queries>();
  Queries& queries = *timer.queries_;
  // Per family, not per device: timestampComputeAndGraphics covers only the
  // families that do both, and a library may be handed a compute-only one.
  queries.valid_bits = device.timestamp_valid_bits();
  queries.period_ns = device.caps().limits().timestampPeriod;
  queries.calibrator = detail::DeviceAccess::calibrator(device);
  if (queries.valid_bits == 0 || queries.period_ns <= 0.0F) {
    queries.valid_bits = 0;
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
    queries.valid_bits = 0;
    return timer;
  }
  queries.pool = *std::move(pool);
  return timer;
}

bool GpuTimer::available() const noexcept {
  return queries_ != nullptr && queries_->valid_bits != 0 &&
         queries_->pool.valid();
}

std::shared_ptr<void> GpuTimer::keep_alive() const noexcept { return queries_; }

Status GpuTimer::reserve(const Device& device, std::uint32_t max_spans) {
  if (valid() && device.handle() != device_) {
    return Status::invalid_argument(
        "GpuTimer::reserve: not the device the timer was made for");
  }
  const std::uint32_t wanted = std::min(max_spans, kMaxSpans);
  if (!available() || wanted * 2 <= queries_->pool.query_count()) return {};
  Result<QueryPool> pool = QueryPool::create(device_, wanted * 2);
  if (!pool) {
    // A larger pool failing is no reason to stop timing at the size that
    // works: the timer keeps its own.
    log_message(LogLevel::Warning, kLogSource,
                "GpuTimer::reserve: " + pool.status().message() + "; keeping " +
                    std::to_string(queries_->pool.query_count() / 2) +
                    " spans");
    return {};
  }
  if (max_spans > kMaxSpans) {
    log_message(LogLevel::Warning, kLogSource,
                "GpuTimer::reserve: " + std::to_string(max_spans) +
                    " spans asked, " + std::to_string(kMaxSpans) +
                    " kept (kMaxSpans)");
  }
  // A new pool beside the old, which the batches pending on it keep until
  // they have read their ranges.
  auto grown = std::make_shared<Queries>();
  grown->valid_bits = queries_->valid_bits;
  grown->period_ns = queries_->period_ns;
  grown->calibrator = queries_->calibrator;
  grown->pool = *std::move(pool);
  queries_ = std::move(grown);
  return {};
}

// --- GpuTiles ----------------------------------------------------------------

GpuTiles::~GpuTiles() { release(); }

GpuTiles::GpuTiles(GpuTiles&& other) noexcept
    : queries_(std::move(other.queries_)),
      first_(std::exchange(other.first_, 0)),
      capacity_(std::exchange(other.capacity_, 0)),
      written_(std::exchange(other.written_, 0)),
      depth_(std::exchange(other.depth_, 0)),
      untimed_(std::exchange(other.untimed_, DeviceTime::None)),
      overflowed_(std::exchange(other.overflowed_, false)),
      ended_(std::exchange(other.ended_, false)),
      leaked_(std::exchange(other.leaked_, false)),
      settled_(std::exchange(other.settled_, false)),
      keys_(std::exchange(other.keys_, {})),
      tiles_(std::exchange(other.tiles_, {})),
      tile_ms_(std::exchange(other.tile_ms_, {})),
      tile_state_(std::exchange(other.tile_state_, {})),
      rows_(std::exchange(other.rows_, StageMetrics{})),
      span_(std::exchange(other.span_, std::nullopt)) {}

GpuTiles& GpuTiles::operator=(GpuTiles&& other) noexcept {
  if (this != &other) {
    release();
    queries_ = std::move(other.queries_);
    first_ = std::exchange(other.first_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
    written_ = std::exchange(other.written_, 0);
    depth_ = std::exchange(other.depth_, 0);
    untimed_ = std::exchange(other.untimed_, DeviceTime::None);
    overflowed_ = std::exchange(other.overflowed_, false);
    ended_ = std::exchange(other.ended_, false);
    leaked_ = std::exchange(other.leaked_, false);
    settled_ = std::exchange(other.settled_, false);
    keys_ = std::exchange(other.keys_, {});
    tiles_ = std::exchange(other.tiles_, {});
    tile_ms_ = std::exchange(other.tile_ms_, {});
    tile_state_ = std::exchange(other.tile_state_, {});
    rows_ = std::exchange(other.rows_, StageMetrics{});
    span_ = std::exchange(other.span_, std::nullopt);
  }
  return *this;
}

GpuTiles GpuTiles::begin(GpuTimer& timer, VkCommandBuffer cmd,
                         std::uint32_t tiles) {
  return open(timer, cmd, tiles, {});
}

GpuTiles GpuTiles::open(GpuTimer& timer, VkCommandBuffer cmd,
                        std::uint32_t tiles,
                        std::vector<detail::TileKey> keys) {
  GpuTiles out;
  out.keys_ = std::move(keys);
  out.depth_ = timer.max_depth();
  out.tiles_.push_back(kNoKey);  // the open tile, unnamed until a mark
  if (!timer.available() || cmd == VK_NULL_HANDLE) {
    out.untimed_ = DeviceTime::Unavailable;
    return out;
  }
  // One mark more than tiles; a batch of more than the pool holds finds no
  // run, as a full pool does (and one at the ceiling never fits either).
  const std::uint32_t room = std::max(tiles, 1U);
  const std::uint32_t marks =
      room == std::numeric_limits<std::uint32_t>::max() ? room : room + 1;
  const std::optional<std::uint32_t> first = timer.queries_->acquire(marks);
  if (!first.has_value()) {
    out.untimed_ = DeviceTime::NotTimed;
    return out;
  }
  out.queries_ = timer.queries_;
  out.first_ = *first;
  out.capacity_ = marks;
  // One reset for the range, at the start, before any mark: a reset between
  // two commands can split a Metal compute encoder, perturbing what is
  // measured.
  out.queries_->pool.cmd_reset(cmd, out.first_, marks);
  // BOTTOM_OF_PIPE, as every mark: written once the earlier work on the
  // queue completes, so a tile never starts while another library's work
  // drains, and the tiles of a command buffer tile rather than nest.
  out.queries_->pool.cmd_write_timestamp(
      cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, out.first_);
  out.written_ = 1;
  return out;
}

GpuTiles GpuTiles::refused(const GpuTimer& timer,
                           std::vector<detail::TileKey> keys) {
  GpuTiles out;
  out.keys_ = std::move(keys);
  out.depth_ = timer.max_depth();
  out.untimed_ =
      timer.available() ? DeviceTime::NotTimed : DeviceTime::Unavailable;
  for (std::size_t k = 0; k < out.keys_.size(); ++k) {
    out.tiles_.push_back(static_cast<std::uint32_t>(k));
  }
  out.settle(nullptr);
  return out;
}

void GpuTiles::mark(VkCommandBuffer cmd, const char* name) {
  VKC_CHECK(name != nullptr, "GpuTiles::mark: the tile's name is null");
  // A tile named by hand is a key of its name alone.
  const auto named = [name](const detail::TileKey& key) {
    return key.names.size() == 1 && key.serials[0] == 0 &&
           std::strcmp(key.names[0], name) == 0;
  };
  const auto found = std::find_if(keys_.begin(), keys_.end(), named);
  const auto key = static_cast<std::uint32_t>(found - keys_.begin());
  if (found == keys_.end()) keys_.push_back(detail::TileKey{{name}, {0}});
  mark_key(cmd, key);
}

void GpuTiles::mark_key(VkCommandBuffer cmd, std::uint32_t key) {
  if (tiles_.empty() || settled_) return;  // never opened, or done
  if (tiles_.back() == kNoKey) {
    tiles_.back() = key;  // the first name names the open tile
    return;
  }
  if (same_tile(keys_, depth_, tiles_.back(), key)) return;
  if (queries_ != nullptr && !overflowed_) {
    // Room is kept for the last mark.
    if (written_ + 2 > capacity_) {
      overflowed_ = true;
    } else {
      queries_->pool.cmd_write_timestamp(
          cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, first_ + written_);
      ++written_;
    }
  }
  tiles_.push_back(key);
}

void GpuTiles::end(VkCommandBuffer cmd) {
  if (ended_ || settled_) return;
  ended_ = true;
  if (queries_ == nullptr || written_ >= capacity_) return;
  queries_->pool.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     first_ + written_);
  ++written_;
}

bool GpuTiles::same_tile(const std::vector<detail::TileKey>& keys,
                         std::uint32_t depth, std::uint32_t a,
                         std::uint32_t b) noexcept {
  if (a == b) return true;
  const detail::TileKey& x = keys[a];
  const detail::TileKey& y = keys[b];
  const auto deep = [depth](const detail::TileKey& key) {
    return depth == 0 ? key.names.size()
                      : std::min<std::size_t>(depth, key.names.size());
  };
  const std::size_t n = deep(x);
  if (n != deep(y)) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (x.serials[i] != y.serials[i] ||
        std::strcmp(x.names[i], y.names[i]) != 0) {
      return false;
    }
  }
  return true;
}

template <class Fn>
void GpuTiles::for_each_credit(const Fn& fn) const {
  const auto depth = [this](const detail::TileKey& key) {
    return depth_ == 0 ? key.names.size()
                       : std::min<std::size_t>(depth_, key.names.size());
  };
  // Each tile counts to its key's scopes, down to the depth...
  for (std::size_t t = 0; t < tiles_.size() && t < tile_state_.size(); ++t) {
    if (tiles_[t] == kNoKey) continue;
    const detail::TileKey& key = keys_[tiles_[t]];
    for (std::size_t i = 0; i < depth(key); ++i) {
      fn(key.names[i], key.serials[i], tile_state_[t], tile_ms_[t]);
    }
  }
  // ...and the scopes deeper than it ran untimed.
  const DeviceTime deeper = untimed_ == DeviceTime::Unavailable
                                ? DeviceTime::Unavailable
                                : DeviceTime::NotTimed;
  for (const detail::TileKey& key : keys_) {
    for (std::size_t i = depth(key); i < key.names.size(); ++i) {
      fn(key.names[i], key.serials[i], deeper, 0.0);
    }
  }
}

void GpuTiles::settle(const std::uint64_t* marks) {
  settled_ = true;
  const std::size_t n = tiles_.size();
  const DeviceTime fallback =
      untimed_ != DeviceTime::None ? untimed_ : DeviceTime::NotTimed;
  tile_ms_.assign(n, 0.0);
  tile_state_.assign(n, fallback);
  // Every tile has its two marks only when nothing overflowed and the last
  // was written; a mark not yet readable leaves its tiles unmeasured.
  const bool marked =
      marks != nullptr && ended_ && !overflowed_ && written_ == n + 1;
  if (marked) {
    for (std::size_t t = 0; t < n; ++t) {
      const std::uint64_t* a = marks + (2 * t);
      const std::uint64_t* b = marks + (2 * (t + 1));
      if (a[1] == 0 || b[1] == 0) continue;
      tile_ms_[t] =
          ticks_to_ms(timestamp_delta(a[0], b[0], queries_->valid_bits),
                      queries_->period_ns);
      tile_state_[t] = DeviceTime::Measured;
    }
  }
  rows_.clear();
  for_each_credit([this](const char* name, std::uint64_t, DeviceTime state,
                         double ms) { rows_.add_device(name, state, ms); });
  // The span needs only the two ends.
  const std::size_t last = written_ == 0 ? 0 : std::size_t{written_} - 1;
  if (marks != nullptr && ended_ && written_ >= 2 && marks[1] != 0 &&
      marks[(2 * last) + 1] != 0) {
    DeviceSpan span;
    span.begin_ticks = marks[0];
    span.end_ticks = marks[2 * last];
    if (const std::optional<ClockPair> pair = queries_->clock_pair()) {
      const auto host = [&](std::uint64_t ticks) {
        const double ns =
            static_cast<double>(
                signed_ticks(pair->device_ticks, ticks, queries_->valid_bits)) *
            static_cast<double>(queries_->period_ns);
        return pair->host_ns + static_cast<std::int64_t>(std::llround(ns));
      };
      span.begin_ns = host(span.begin_ticks);
      span.end_ns = host(span.end_ticks);
      span.deviation_ns = pair->deviation_ns;
      span.host_clock = true;
    }
    span_ = span;
  }
}

void GpuTiles::resolve() {
  if (settled_) return;
  if (queries_ == nullptr || written_ == 0) {
    settle(nullptr);
    release();
    return;
  }
  std::vector<std::uint64_t> marks(static_cast<std::size_t>(written_) * 2);
  const Status read =
      queries_->pool.read_results(first_, written_, marks.data(),
                                  /*with_availability=*/true);
  if (read.ok()) {
    settle(marks.data());
  } else {
    // Logged, not returned: the work succeeded, and failing it over a
    // diagnostic is what this class exists not to do.
    log_message(LogLevel::Warning, kLogSource,
                "GpuTiles: timestamps not read (" + read.message() +
                    "); the work itself succeeded");
    untimed_ = DeviceTime::NotTimed;
    settle(nullptr);
  }
  release();
}

void GpuTiles::drop(bool may_still_run) {
  if (settled_) return;
  // Work the device may still run may still write every query in the range,
  // which no later batch may then reset (VUID-vkCmdResetQueryPool-None-02841).
  leaked_ = may_still_run;
  if (untimed_ == DeviceTime::None) untimed_ = DeviceTime::NotTimed;
  settle(nullptr);
  release();
}

void GpuTiles::publish() const noexcept {
  for_each_credit(
      [](const char*, std::uint64_t serial, DeviceTime state, double ms) {
        if (serial == 0) return;
        for (detail::ScopeFrame* frame = detail::innermost_scope();
             frame != nullptr; frame = frame->outer) {
          if (frame->serial != serial) continue;
          if (state == DeviceTime::Measured) frame->device_ms += ms;
          frame->device = std::max(frame->device, state);
          return;
        }
      });
}

void GpuTiles::release() noexcept {
  if (queries_ == nullptr) return;
  if (!leaked_) queries_->release(first_);
  queries_.reset();
}

}  // namespace volumetric_kit::core
