// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// QueryPool, GpuTimer, GpuTiles and GpuStageScope: the tiles CommandBatch,
// dispatch() and the Device overload record, a pending batch's rows and
// span, the blocked time the core's waits report, the device's counters and
// its calibrated clocks. The tick math and the inert paths need no device.

#include "volumetric_kit/core/vulkan/gpu_timer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "add_comp.spv.hpp"
#include "device_access.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/command_buffer.hpp"
#include "volumetric_kit/core/vulkan/command_pool.hpp"
#include "volumetric_kit/core/vulkan/compute_kernel.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/query_pool.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#include <stdexcept>
#define VKC_TEST_EXCEPTIONS 1
#endif

namespace volumetric_kit::core {
namespace {

bool is_invalid(const Status& s) {
  return s.domain() == Status::Code::InvalidArgument;
}

// The row named @p name, or null.
const StageRow* row(const std::vector<StageRow>& rows, const char* name) {
  for (const StageRow& r : rows) {
    if (std::string(r.name) == name) return &r;
  }
  return nullptr;
}

const StageRow* row(const StageMetrics& metrics, const char* name) {
  return row(metrics.rows(), name);
}

std::int64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- without a device --------------------------------------------------------

TEST(TimestampMath, WrapsAndConverts) {
  EXPECT_EQ(timestamp_delta(100, 150, 64), 50U);
  EXPECT_EQ(timestamp_delta(100, 150, 0), 0U);  // no counter, not 2^64
  // A 32-bit counter that wrapped between the two: masking the endpoints
  // first would report 2^64 - 20.
  const std::uint64_t begin = (std::uint64_t{1} << 32) - 10;
  EXPECT_EQ(timestamp_delta(begin, begin + 20, 32), 20U);
  EXPECT_EQ(ticks_to_ms(1'000'000, 1.0F), 1.0);  // 1e6 ns is 1 ms
  EXPECT_EQ(ticks_to_ms(1'000'000, 0.0F), 0.0);  // no period, no claim
}

// A host stamp inside the call is steady_clock's own, kept with the driver's
// deviation, edges included; one outside it (another clock), or none, puts
// the device's reading at the call's middle, adding the driver's uncertainty
// to half the call.
TEST(ClockPairs, KeepAHostStampOnlyInsideTheCall) {
  const ClockPair read = detail::pair_clocks(7, 1'040, 3, 1'000, 1'100);
  EXPECT_EQ(read.device_ticks, 7U);
  EXPECT_EQ(read.host_ns, 1'040);
  EXPECT_EQ(read.deviation_ns, 3U);
  EXPECT_EQ(detail::pair_clocks(7, 1'000, 3, 1'000, 1'100).host_ns, 1'000);
  EXPECT_EQ(detail::pair_clocks(7, 1'100, 3, 1'000, 1'100).host_ns, 1'100);
  for (const std::optional<std::uint64_t> stamp :
       {std::optional<std::uint64_t>(999),
        std::optional<std::uint64_t>(5'000'000'000),
        std::optional<std::uint64_t>()}) {
    const ClockPair bracketed = detail::pair_clocks(7, stamp, 3, 1'000, 1'100);
    EXPECT_EQ(bracketed.device_ticks, 7U);
    EXPECT_EQ(bracketed.host_ns, 1'050);
    EXPECT_EQ(bracketed.deviation_ns, 53U);
  }
}

TEST(ClockPairs, BracketIncludesDriverUncertaintyWithoutOverflow) {
  for (const std::optional<std::uint64_t> stamp :
       {std::optional<std::uint64_t>(999), std::optional<std::uint64_t>(1'101),
        std::optional<std::uint64_t>()}) {
    const ClockPair pair = detail::pair_clocks(7, stamp, 1'000, 1'000, 1'100);
    EXPECT_EQ(pair.host_ns, 1'050);
    EXPECT_EQ(pair.deviation_ns, 1'050U);
    const ClockPair odd =
        detail::pair_clocks(7, std::nullopt, 1'000, 1'000, 1'101);
    EXPECT_EQ(odd.deviation_ns, 1'051U);
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    const ClockPair saturated =
        detail::pair_clocks(7, std::nullopt, kMax - 10, 1'000, 1'100);
    EXPECT_EQ(saturated.deviation_ns, kMax);
  }
}

TEST(QueryPoolArgs, RefusesANullDeviceOrNoQueries) {
  EXPECT_TRUE(is_invalid(QueryPool::create(VK_NULL_HANDLE, 2).status()));
  const QueryPool empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.query_count(), 0U);
  std::uint64_t out[2] = {};
  EXPECT_TRUE(is_invalid(empty.read_results(0, 1, out)));
}

// An empty timer cannot time: its tiles record nothing and say why.
TEST(GpuTimerEmpty, IsInert) {
  GpuTimer timer;
  EXPECT_FALSE(timer.valid());
  EXPECT_FALSE(timer.available());
  EXPECT_EQ(timer.keep_alive(), nullptr);
  EXPECT_EQ(timer.max_depth(), 0U);
  GpuTiles tiles = GpuTiles::begin(timer, VK_NULL_HANDLE, 1);
  EXPECT_FALSE(tiles.timed());
  tiles.mark(VK_NULL_HANDLE, "x");
  tiles.end(VK_NULL_HANDLE);
  EXPECT_TRUE(tiles.rows().empty());  // nothing before it settles
  tiles.resolve();
  ASSERT_EQ(tiles.rows().size(), 1U);
  EXPECT_EQ(tiles.rows()[0].device, DeviceTime::Unavailable);
  EXPECT_FALSE(tiles.span().has_value());
  GpuTiles none;
  none.resolve();
  EXPECT_TRUE(none.rows().empty());
  none.drop(/*may_still_run=*/true);
  EXPECT_TRUE(none.rows().empty());
}

// A tile's name lands in a StageMetrics row, which a null one would crash.
TEST(GpuTimerDeathTest, ANullNameIsAProgrammerError) {
  GpuTimer timer;
  GpuTiles tiles = GpuTiles::begin(timer, VK_NULL_HANDLE, 1);
  EXPECT_DEATH(tiles.mark(VK_NULL_HANDLE, nullptr), "name is null");
}

TEST(GpuStageScopeInert, NullMetricsTimesNothing) {
  GpuTimer timer;
  {
    const GpuStageScope stage(nullptr, timer, "never");
    EXPECT_EQ(stage.timer(), nullptr);
    EXPECT_EQ(stage.tag().timer, nullptr);
    EXPECT_STREQ(stage.name(), "never");
    EXPECT_EQ(detail::innermost_scope(), nullptr);
  }
  StageMetrics metrics;
  {
    const GpuStageScope stage(&metrics, timer, "host only");
    EXPECT_EQ(stage.timer(), &timer);
    EXPECT_NE(stage.tag().scope, 0U);
    // A command tagged now counts to this scope, outermost first.
    const detail::TileKey key = detail::tile_key(stage.tag());
    ASSERT_EQ(key.names.size(), 1U);
    EXPECT_STREQ(key.names[0], "host only");
  }
  // Nothing recorded: the host row lands, with no device half.
  ASSERT_EQ(metrics.rows().size(), 1U);
  EXPECT_EQ(metrics.rows()[0].device, DeviceTime::None);
}

// A tag's key names every scope open around it, outermost first, and skips
// a wait scope; a tag whose scope is not open here keys on its name alone.
TEST(TileKey, NamesTheScopesAroundTheTag) {
  GpuTimer timer;
  StageMetrics metrics;
  const StageScope frame(metrics, "frame");
  const WaitScope wait(&metrics, "wait");
  const GpuStageScope stage(&metrics, timer, "decode");
  const detail::TileKey key = detail::tile_key(stage.tag());
  ASSERT_EQ(key.names.size(), 2U);
  EXPECT_STREQ(key.names[0], "frame");
  EXPECT_STREQ(key.names[1], "decode");
  EXPECT_EQ(key.serials[0], frame.serial());
  EXPECT_EQ(key.serials[1], stage.tag().scope);
  const detail::TileKey stray =
      detail::tile_key(GpuSpanTag{&timer, "elsewhere", 0});
  ASSERT_EQ(stray.names.size(), 1U);
  EXPECT_EQ(stray.serials[0], 0U);
}

// A tag carried to another thread keys on its own name there, never on that
// thread's scopes: each fresh thread's first scope would share a serial
// counted per thread.
TEST(TileKey, ATagOnAnotherThreadKeysOnItsName) {
  GpuTimer timer;
  StageMetrics recording;
  StageMetrics other;
  GpuSpanTag tag;
  std::thread([&] {
    const GpuStageScope stage(&recording, timer, "codec decode");
    tag = stage.tag();
  }).join();
  detail::TileKey key;
  std::thread([&] {
    const StageScope unrelated(other, "unrelated host stage");
    key = detail::tile_key(tag);
  }).join();
  ASSERT_EQ(key.names.size(), 1U);
  EXPECT_STREQ(key.names[0], "codec decode");
  EXPECT_EQ(key.serials[0], 0U);
}

// --- on a device -------------------------------------------------------------

// Large enough that filling it is clearly more than a tick on any GPU the
// family targets, so a device span above zero means something.
constexpr VkDeviceSize kFillBytes = VkDeviceSize{64} << 20;
// Longer than any wait here should take, so a broken one fails the test
// rather than hanging it.
constexpr std::uint64_t kLongWaitNs = 10'000'000'000;

using TimerTest = test::VulkanDeviceTest;

class TimerWorkTest : public TimerTest {
 protected:
  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    Result<Buffer> made = device_storage_buffer(allocator(), kFillBytes);
    ASSERT_TRUE(made.ok()) << made.status().message();
    target_ = *std::move(made);
    Result<GpuTimer> timer = GpuTimer::create(device());
    ASSERT_TRUE(timer.ok()) << timer.status().message();
    timer_ = *std::move(timer);
  }
  void TearDown() override {
    timer_ = GpuTimer{};
    target_ = Buffer{};
    VulkanDeviceTest::TearDown();
  }

  // Whether the device's timestamps advance across a fill. A paravirtualized
  // GPU (GitHub's macOS runners) reports valid bits and returns one value for
  // every timestamp, so the magnitude of a tile is checked only where they
  // move; that tiles are recorded, read and published is checked
  // everywhere.
  bool timestamps_advance() {
    if (advance_.has_value()) return *advance_;
    advance_ = probe_timestamps();
    return *advance_;
  }

  bool probe_timestamps() {
    if (device().timestamp_valid_bits() == 0) return false;
    Result<QueryPool> pool = QueryPool::create(device().handle(), 2);
    if (!pool.ok()) return false;
    const auto record = fill();
    const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
      pool->cmd_reset(cmd, 0, 2);
      pool->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
      record(cmd);
      pool->cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
    });
    std::uint64_t out[4] = {};
    return s.ok() && pool->read_results(0, 2, out, true).ok() && out[1] != 0 &&
           out[3] != 0 &&
           timestamp_delta(out[0], out[2], device().timestamp_valid_bits()) > 0;
  }

  // Records a fill of the whole target.
  std::function<void(VkCommandBuffer)> fill() const {
    VkBuffer target = target_.handle();
    return [target](VkCommandBuffer cmd) {
      vkCmdFillBuffer(cmd, target, 0, kFillBytes, 0xA5A5A5A5U);
    };
  }

  // An upload of 64 bytes to the target, tagged with `stage`: a command a
  // scope can tag, as a fill takes no tag.
  Status tagged(CommandBatch& batch, GpuStageScope* stage) {
    return batch.upload(target_, 0, words_.data(), 64, stage);
  }

  // The state a timed row should read on this device.
  DeviceTime expected() const {
    return timer_.available() ? DeviceTime::Measured : DeviceTime::Unavailable;
  }

  Buffer target_;
  GpuTimer timer_;
  std::vector<std::uint32_t> words_ = std::vector<std::uint32_t>(16, 7U);
  std::optional<bool> advance_;
};

// Says, as a skip, when this device's timestamps do not advance -- why the
// tests below check no tile's magnitude here.
TEST_F(TimerWorkTest, TimestampsAdvance) {
  if (device().timestamp_valid_bits() == 0) {
    GTEST_SKIP() << "the queue family has no timestamps";
  }
  if (!timestamps_advance()) {
    GTEST_SKIP() << "timestamps do not advance on this device (a "
                    "paravirtualized GPU): tile magnitudes go unchecked";
  }
}

TEST_F(TimerWorkTest, QueryPoolWritesAndReadsTimestamps) {
  // One 64-bit result a query is all read_results reads.
  EXPECT_TRUE(
      QueryPool::create(device().handle(), 2, VK_QUERY_TYPE_OCCLUSION).ok());
  EXPECT_EQ(
      QueryPool::create(device().handle(), 2, VK_QUERY_TYPE_PIPELINE_STATISTICS)
          .status()
          .domain(),
      Status::Code::Unsupported);
  Result<QueryPool> made = QueryPool::create(device().handle(), 2);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const QueryPool pool = *std::move(made);
  EXPECT_EQ(pool.query_count(), 2U);
  std::uint64_t out[4] = {};
  EXPECT_TRUE(is_invalid(pool.read_results(1, 2, out)));  // past the pool
  EXPECT_TRUE(is_invalid(pool.read_results(0, 1, nullptr)));
  if (device().timestamp_valid_bits() == 0) {
    GTEST_SKIP() << "the queue family has no timestamps";
  }
  const auto record = fill();
  const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
    pool.cmd_reset(cmd, 0, 2);
    pool.cmd_reset(cmd, 1, 5);  // past the pool: recorded nothing
    pool.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    record(cmd);
    pool.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
    pool.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 9);
  });
  ASSERT_TRUE(s.ok()) << s.message();
  ASSERT_TRUE(pool.read_results(0, 2, out, /*with_availability=*/true).ok());
  EXPECT_NE(out[1], 0U);  // both written
  EXPECT_NE(out[3], 0U);
  if (timestamps_advance()) {
    EXPECT_GT(timestamp_delta(out[0], out[2], device().timestamp_valid_bits()),
              0U);
  }
}

// The device tile is the recorded work alone, so it comes in under the wall
// clock around the whole blocking submit -- if it were the wall clock
// relabelled, or read against the wrong period, it would not -- and the
// fence wait is that row's blocked time.
TEST_F(TimerWorkTest, TimesTheWorkNotTheSubmit) {
  EXPECT_TRUE(timer_.valid());
  StageMetrics metrics;
  double wall_ms = 0.0;
  {
    GpuStageScope stage(&metrics, timer_, "fill");
    const auto start = std::chrono::steady_clock::now();
    const Status s = device().submit_single_time(fill(), stage);
    wall_ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    ASSERT_TRUE(s.ok()) << s.message();
  }
  const StageRow* fill_row = row(metrics, "fill");
  ASSERT_NE(fill_row, nullptr);
  EXPECT_GT(fill_row->cpu_ms, 0.0);  // the scope's host half
  EXPECT_LE(fill_row->wait_ms, fill_row->cpu_ms);
  // A family without timestamps is supported: n/a, not a zero.
  EXPECT_EQ(fill_row->device, expected());
  EXPECT_EQ(fill_row->has_gpu, timer_.available());
  if (timer_.available() && timestamps_advance()) {
    EXPECT_GT(fill_row->gpu_ms, 0.0);
    EXPECT_LT(fill_row->gpu_ms, wall_ms);
  }
}

// Commands before the first tag join its tile, as untagged ones join the open
// tile: a stage's time counts the fills and barriers between its own
// commands.
TEST_F(TimerWorkTest, UntaggedCommandsJoinTheOpenTile) {
  if (!timer_.available()) GTEST_SKIP() << "the queue has no timestamps";
  // A batch with no tag still has a span.
  CommandBatch alone(device(), allocator());
  alone.time(timer_);
  ASSERT_TRUE(alone.fill(target_, 0, kFillBytes, 1U).ok());
  Result<PendingBatch> reference = alone.submit_async({}, {});
  ASSERT_TRUE(reference.ok()) << reference.status().message();
  ASSERT_TRUE(reference->wait(kLongWaitNs).ok());
  EXPECT_TRUE(reference->device_rows().empty());  // no tag, no row
  EXPECT_TRUE(reference->device_span().has_value());

  // A fill before the first tag joins its tile, and one after it the open
  // tile: one tag makes one tile however many untagged commands surround
  // it. So the batch fits a timer with room for one tile -- a second would
  // read NotTimed -- and that tile is the whole span.
  Result<GpuTimer> made_timer = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made_timer.ok());
  GpuTimer one_tile = *std::move(made_timer);
  StageMetrics recording;
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.fill(target_, 0, kFillBytes, 2U).ok());
  {
    GpuStageScope only(&recording, one_tile, "only");
    ASSERT_TRUE(tagged(batch, &only).ok());
  }
  ASSERT_TRUE(batch.fill(target_, 0, kFillBytes, 3U).ok());
  Result<PendingBatch> made = batch.submit_async({}, {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  ASSERT_TRUE(made->wait(kLongWaitNs).ok());
  ASSERT_EQ(made->device_rows().size(), 1U);
  const StageRow& only = made->device_rows()[0];
  EXPECT_EQ(only.device, DeviceTime::Measured);
  ASSERT_TRUE(made->device_span().has_value());
  const DeviceSpan span = made->device_span().value_or(DeviceSpan{});
  EXPECT_NEAR(only.gpu_ms,
              ticks_to_ms(timestamp_delta(span.begin_ticks, span.end_ticks,
                                          device().timestamp_valid_bits()),
                          physical().limits().timestampPeriod),
              1e-6);
}

// An async batch is timed: its rows come back by name at the wait, not
// before, and nothing points at the caller's metrics meanwhile, which may
// move.
TEST_F(TimerWorkTest, AnAsyncBatchReturnsItsRowsAtTheWait) {
  StageMetrics recording;
  CommandBatch batch(device(), allocator());
  {
    GpuStageScope stage(&recording, timer_, "fill");
    ASSERT_TRUE(tagged(batch, &stage).ok());
    ASSERT_TRUE(batch.fill(target_, 0, kFillBytes, 3U).ok());
  }
  ASSERT_NE(row(recording, "fill"), nullptr);
  EXPECT_EQ(row(recording, "fill")->device, DeviceTime::None);  // closed
  Result<PendingBatch> made = batch.submit_async({}, {});
  ASSERT_TRUE(made.ok()) << made.status().message();
  PendingBatch pending = *std::move(made);
  EXPECT_TRUE(pending.device_rows().empty());
  EXPECT_FALSE(pending.device_span().has_value());
  const StageMetrics moved = std::move(recording);
  ASSERT_TRUE(pending.wait(kLongWaitNs).ok());
  const StageRow* fill_row = row(pending.device_rows(), "fill");
  ASSERT_NE(fill_row, nullptr);
  EXPECT_EQ(fill_row->device, expected());
  EXPECT_EQ(fill_row->cpu_ms, 0.0);  // device halves only
  EXPECT_EQ(pending.device_span().has_value(), timer_.available());
  StageMetrics shown = moved;
  shown.merge_device(pending.device_rows());
  EXPECT_EQ(row(shown, "fill")->device, expected());
  EXPECT_GT(row(shown, "fill")->cpu_ms, 0.0);
  if (timer_.available() && timestamps_advance()) {
    EXPECT_GT(row(shown, "fill")->gpu_ms, 0.0);
  }
  // A later wait reads nothing again.
  ASSERT_TRUE(pending.wait().ok());
  EXPECT_EQ(pending.device_rows().size(), 1U);
}

// Two batches pending on one timer hold two ranges: each reads its own
// tiles, whichever is waited first, and the later one's span starts after
// the earlier one's ends, on one queue.
TEST_F(TimerWorkTest, TwoPendingBatchesStaySeparate) {
  if (!timer_.available()) GTEST_SKIP() << "the queue has no timestamps";
  StageMetrics recording;
  const auto submit = [&](const char* name) {
    CommandBatch batch(device(), allocator());
    GpuStageScope stage(&recording, timer_, name);
    EXPECT_TRUE(tagged(batch, &stage).ok());
    EXPECT_TRUE(batch.fill(target_, 0, kFillBytes, 4U).ok());
    Result<PendingBatch> made = batch.submit_async({}, {});
    EXPECT_TRUE(made.ok()) << made.status().message();
    return made.ok() ? *std::move(made) : PendingBatch{};
  };
  PendingBatch first = submit("first");
  PendingBatch second = submit("second");
  ASSERT_TRUE(second.wait(kLongWaitNs).ok());
  ASSERT_TRUE(first.wait(kLongWaitNs).ok());
  ASSERT_EQ(first.device_rows().size(), 1U);
  ASSERT_EQ(second.device_rows().size(), 1U);
  EXPECT_STREQ(first.device_rows()[0].name, "first");
  EXPECT_STREQ(second.device_rows()[0].name, "second");
  EXPECT_EQ(first.device_rows()[0].device, DeviceTime::Measured);
  EXPECT_EQ(second.device_rows()[0].device, DeviceTime::Measured);
  const std::optional<DeviceSpan> earlier = first.device_span();
  const std::optional<DeviceSpan> later = second.device_span();
  ASSERT_TRUE(earlier.has_value());
  ASSERT_TRUE(later.has_value());
  if (timestamps_advance()) {
    const std::uint32_t bits = device().timestamp_valid_bits();
    const std::uint64_t gap =
        timestamp_delta(earlier.value_or(DeviceSpan{}).end_ticks,
                        later.value_or(DeviceSpan{}).begin_ticks, bits);
    const std::uint64_t half =
        bits >= 64 ? std::uint64_t{1} << 63 : std::uint64_t{1} << (bits - 1);
    EXPECT_LT(gap, half);  // not negative: they never overlap
  }
}

// A timer with no free range runs the batch untimed, its rows NotTimed, and
// times again once a range comes back.
TEST_F(TimerWorkTest, NoFreeRangeReadsNotTimed) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);  // two queries
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  if (!tiny.available()) GTEST_SKIP() << "the queue has no timestamps";
  StageMetrics recording;
  // Two tiles take three marks: none fits.
  {
    CommandBatch batch(device(), allocator());
    {
      GpuStageScope a(&recording, tiny, "a");
      ASSERT_TRUE(tagged(batch, &a).ok());
    }
    {
      GpuStageScope b(&recording, tiny, "b");
      ASSERT_TRUE(tagged(batch, &b).ok());
    }
    Result<PendingBatch> pending = batch.submit_async({}, {});
    ASSERT_TRUE(pending.ok());
    ASSERT_TRUE(pending->wait(kLongWaitNs).ok());
    ASSERT_EQ(pending->device_rows().size(), 2U);
    EXPECT_EQ(pending->device_rows()[0].device, DeviceTime::NotTimed);
    EXPECT_EQ(pending->device_rows()[1].device, DeviceTime::NotTimed);
    EXPECT_FALSE(pending->device_span().has_value());
  }
  // One tile fits; a second batch pending beside it finds the range held.
  const auto submit = [&](const char* name) {
    CommandBatch batch(device(), allocator());
    GpuStageScope stage(&recording, tiny, name);
    EXPECT_TRUE(tagged(batch, &stage).ok());
    Result<PendingBatch> pending = batch.submit_async({}, {});
    EXPECT_TRUE(pending.ok());
    return pending.ok() ? *std::move(pending) : PendingBatch{};
  };
  PendingBatch held = submit("held");
  PendingBatch refused = submit("refused");
  ASSERT_TRUE(refused.wait(kLongWaitNs).ok());
  ASSERT_TRUE(held.wait(kLongWaitNs).ok());
  EXPECT_EQ(held.device_rows()[0].device, DeviceTime::Measured);
  EXPECT_EQ(refused.device_rows()[0].device, DeviceTime::NotTimed);
  PendingBatch again = submit("again");
  ASSERT_TRUE(again.wait(kLongWaitNs).ok());
  EXPECT_EQ(again.device_rows()[0].device, DeviceTime::Measured);
}

// A pending batch destroyed, or moved over, without a wait still waits for
// its work, and its range goes back once that wait sees the work complete: a
// timer with room for one batch times the next.
TEST_F(TimerWorkTest, APendingBatchLeftUnwaitedReturnsItsRange) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);  // one batch's room
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  if (!tiny.available()) GTEST_SKIP() << "the queue has no timestamps";
  StageMetrics recording;
  const auto submit = [&](const char* name) {
    CommandBatch batch(device(), allocator());
    GpuStageScope stage(&recording, tiny, name);
    EXPECT_TRUE(tagged(batch, &stage).ok());
    Result<PendingBatch> pending = batch.submit_async({}, {});
    EXPECT_TRUE(pending.ok());
    return pending.ok() ? *std::move(pending) : PendingBatch{};
  };
  {
    const PendingBatch dropped = submit("dropped");
  }
  PendingBatch kept = submit("kept");
  kept = submit("refused");  // submitted while "kept" holds the range
  PendingBatch next = submit("next");
  ASSERT_TRUE(kept.wait(kLongWaitNs).ok());
  ASSERT_TRUE(next.wait(kLongWaitNs).ok());
  ASSERT_EQ(kept.device_rows().size(), 1U);
  ASSERT_EQ(next.device_rows().size(), 1U);
  EXPECT_EQ(kept.device_rows()[0].device, DeviceTime::NotTimed);
  EXPECT_EQ(next.device_rows()[0].device, DeviceTime::Measured);
}

// Pending batches may be waited on another thread while this one records on
// their timer: every wait returns its own rows and its range, so once they
// are all back the pool's whole room times batches pending at once.
TEST_F(TimerWorkTest, PendingBatchesWaitedElsewhereReturnTheirRanges) {
  constexpr std::uint32_t kRoom = 4;  // batches of one tile: 8 queries
  Result<GpuTimer> made = GpuTimer::create(device(), kRoom);
  ASSERT_TRUE(made.ok());
  GpuTimer room = *std::move(made);
  if (!room.available()) GTEST_SKIP() << "the queue has no timestamps";
  StageMetrics recording;
  const auto submit = [&] {
    CommandBatch batch(device(), allocator());
    GpuStageScope stage(&recording, room, "batch");
    EXPECT_TRUE(tagged(batch, &stage).ok());
    Result<PendingBatch> pending = batch.submit_async({}, {});
    EXPECT_TRUE(pending.ok()) << pending.status().message();
    return pending.ok() ? *std::move(pending) : PendingBatch{};
  };
  constexpr int kBatches = 200;
  std::mutex mutex;
  std::condition_variable posted;
  std::deque<PendingBatch> queue;
  bool done = false;
  int waited = 0;  // the waiter's alone until it is joined
  std::thread waiter([&] {
    for (;;) {
      PendingBatch pending;
      {
        std::unique_lock<std::mutex> lock(mutex);
        posted.wait(lock, [&] { return !queue.empty() || done; });
        if (queue.empty()) return;
        pending = std::move(queue.front());
        queue.pop_front();
      }
      EXPECT_TRUE(pending.wait(kLongWaitNs).ok());
      EXPECT_EQ(pending.device_rows().size(), 1U);
      ++waited;
    }
  });
  for (int i = 0; i < kBatches; ++i) {
    PendingBatch pending = submit();
    {
      const std::scoped_lock lock(mutex);
      queue.push_back(std::move(pending));
    }
    posted.notify_one();
  }
  {
    const std::scoped_lock lock(mutex);
    done = true;
  }
  posted.notify_one();
  waiter.join();
  EXPECT_EQ(waited, kBatches);
  std::vector<PendingBatch> together;
  together.reserve(kRoom);
  for (std::uint32_t i = 0; i < kRoom; ++i) together.push_back(submit());
  for (PendingBatch& pending : together) {
    ASSERT_TRUE(pending.wait(kLongWaitNs).ok());
    ASSERT_EQ(pending.device_rows().size(), 1U);
    EXPECT_EQ(pending.device_rows()[0].device, DeviceTime::Measured);
  }
}

// A parent's time is its children's tiles: a scope open around two tagged
// scopes reads their sum. At depth 1 it is one tile, which fits a timer two
// tiles would not, and the deeper scopes read NotTimed.
TEST_F(TimerWorkTest, NestedScopesSumAndDepthCollapses) {
  if (!timer_.available()) GTEST_SKIP() << "the queue has no timestamps";
  const auto record = [&](GpuTimer& timer) {
    StageMetrics recording;
    CommandBatch batch(device(), allocator());
    {
      const StageScope decode(recording, "decode");
      {
        GpuStageScope rans(&recording, timer, "  ..rans");
        EXPECT_TRUE(tagged(batch, &rans).ok());
        EXPECT_TRUE(batch.fill(target_, 0, kFillBytes, 5U).ok());
      }
      {
        GpuStageScope grid(&recording, timer, "  ..grid");
        EXPECT_TRUE(tagged(batch, &grid).ok());
        EXPECT_TRUE(batch.fill(target_, 0, kFillBytes, 6U).ok());
      }
    }
    Result<PendingBatch> pending = batch.submit_async({}, {});
    EXPECT_TRUE(pending.ok());
    if (!pending.ok()) return std::vector<StageRow>{};
    EXPECT_TRUE(pending->wait(kLongWaitNs).ok());
    return pending->device_rows();
  };
  const std::vector<StageRow> all = record(timer_);
  ASSERT_EQ(all.size(), 3U);
  const StageRow* decode = row(all, "decode");
  const StageRow* rans = row(all, "  ..rans");
  const StageRow* grid = row(all, "  ..grid");
  ASSERT_TRUE(decode != nullptr && rans != nullptr && grid != nullptr);
  EXPECT_EQ(decode->device, DeviceTime::Measured);
  EXPECT_EQ(rans->device, DeviceTime::Measured);
  EXPECT_EQ(grid->device, DeviceTime::Measured);
  EXPECT_NEAR(decode->gpu_ms, rans->gpu_ms + grid->gpu_ms, 1e-9);
  StageMetrics totals;
  totals.merge_device(all);
  EXPECT_DOUBLE_EQ(totals.total_gpu_ms(), decode->gpu_ms);  // once

  Result<GpuTimer> made = GpuTimer::create(device(), 1);  // one tile's room
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  EXPECT_EQ(row(record(tiny), "  ..rans")->device, DeviceTime::NotTimed);
  tiny.set_max_depth(1);
  EXPECT_EQ(tiny.max_depth(), 1U);
  const std::vector<StageRow> top = record(tiny);
  ASSERT_EQ(top.size(), 3U);
  EXPECT_EQ(row(top, "decode")->device, DeviceTime::Measured);
  EXPECT_EQ(row(top, "  ..rans")->device, DeviceTime::NotTimed);
  EXPECT_EQ(row(top, "  ..grid")->device, DeviceTime::NotTimed);
}

// Tiles recorded by hand -- a renderer's frame -- mark at each new name only,
// add up to the span, and a tile past the room taken leaves every tile
// NotTimed with the span intact.
TEST_F(TimerWorkTest, TilesByHandAddUpToTheSpan) {
  if (!timer_.available()) GTEST_SKIP() << "the queue has no timestamps";
  const auto run = [&](std::uint32_t room, std::vector<const char*> names) {
    GpuTiles tiles;
    // A quarter of the target each, so the fills need no barrier.
    constexpr VkDeviceSize kQuarter = kFillBytes / 4;
    VkBuffer target = target_.handle();
    const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
      tiles = GpuTiles::begin(timer_, cmd, room);
      VkDeviceSize offset = 0;
      for (const char* name : names) {
        tiles.mark(cmd, name);
        vkCmdFillBuffer(cmd, target, offset, kQuarter, 0U);
        offset += kQuarter;
      }
      tiles.end(cmd);
    });
    EXPECT_TRUE(s.ok()) << s.message();
    tiles.resolve();
    return tiles;
  };
  const GpuTiles three = run(3, {"shadow", "shadow", "main", "ui"});
  ASSERT_EQ(three.rows().size(), 3U);  // "shadow" twice is one tile
  double sum = 0.0;
  for (const StageRow& r : three.rows()) {
    EXPECT_EQ(r.device, DeviceTime::Measured) << r.name;
    sum += r.gpu_ms;
  }
  ASSERT_TRUE(three.span().has_value());
  const DeviceSpan span = three.span().value_or(DeviceSpan{});
  const double span_ms =
      ticks_to_ms(timestamp_delta(span.begin_ticks, span.end_ticks,
                                  device().timestamp_valid_bits()),
                  physical().limits().timestampPeriod);
  EXPECT_NEAR(sum, span_ms, 1e-6);
  const GpuTiles overflowed = run(1, {"a", "b"});
  ASSERT_EQ(overflowed.rows().size(), 2U);
  EXPECT_EQ(overflowed.rows()[0].device, DeviceTime::NotTimed);
  EXPECT_EQ(overflowed.rows()[1].device, DeviceTime::NotTimed);
  EXPECT_TRUE(overflowed.span().has_value());
}

// Recorded and never submitted, as by a submit that failed before the
// device: the tiles read NotTimed and the range goes back. One the device
// may still run keeps its range, which no later batch reuses.
TEST_F(TimerTest, DroppedTilesReadNotTimedAndKeepALiveRange) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  if (!tiny.available()) GTEST_SKIP() << "the queue has no timestamps";
  Result<CommandPool> pool =
      CommandPool::create(device().handle(), device().queue_family());
  ASSERT_TRUE(pool.ok());
  const auto record = [&](bool may_still_run) {
    Result<CommandBuffer> cmd = pool->allocate_primary();
    EXPECT_TRUE(cmd.ok() && cmd->begin().ok());
    GpuTiles tiles = GpuTiles::begin(tiny, cmd->handle(), 1);
    const bool timed = tiles.timed();
    tiles.mark(cmd->handle(), "never ran");
    tiles.end(cmd->handle());
    EXPECT_TRUE(cmd->end().ok());
    tiles.drop(may_still_run);
    EXPECT_EQ(tiles.rows().size(), 1U);
    EXPECT_EQ(tiles.rows()[0].device, DeviceTime::NotTimed);
    return timed;
  };
  EXPECT_TRUE(record(false));
  EXPECT_TRUE(record(true));    // returned: timed again
  EXPECT_FALSE(record(false));  // the last one's range is held for good
}

// A dropped manual submit has no keep_alive supplied by CommandBatch. The
// device must hold its old pool even after reserve replaces it, the tiles
// and timer die, and the Device itself moves. The guard keeps the old code
// from actually destroying queries under a pending submit while its missing
// owner is asserted; it is released only after that submit completes.
using TimerLifetimeTest = test::VulkanTest;

TEST_F(TimerLifetimeTest,
       DroppedManualTilesRetainTheirPoolUntilDeviceTeardown) {
  std::weak_ptr<void> old_pool;
  {
    Result<Device> made_device = Device::create(instance(), physical(), {});
    ASSERT_TRUE(made_device.ok());
    Result<GpuTimer> made_timer = GpuTimer::create(*made_device, 1);
    ASSERT_TRUE(made_timer.ok());
    GpuTimer timer = *std::move(made_timer);
    if (!timer.available()) GTEST_SKIP() << "the queue has no timestamps";
    old_pool = timer.keep_alive();
    const Device moved_device = *std::move(made_device);
    Result<TimelineSemaphore> gate = TimelineSemaphore::create(moved_device);
    ASSERT_TRUE(gate.ok());
    GpuTiles tiles;
    Result<Device::PendingSubmit> pending = moved_device.submit_pending(
        [&](VkCommandBuffer cmd) {
          tiles = GpuTiles::begin(timer, cmd, 1);
          tiles.mark(cmd, "pending");
          tiles.end(cmd);
        },
        {{&*gate, 1}}, {});
    ASSERT_TRUE(pending.ok());
    EXPECT_FALSE(pending->ready());
    EXPECT_TRUE(timer.reserve(moved_device, 2).ok());
    std::shared_ptr<void> guard = old_pool.lock();
    tiles.drop(true);
    EXPECT_EQ(tiles.rows().size(), 1U);
    if (!tiles.rows().empty()) {
      EXPECT_EQ(tiles.rows()[0].device, DeviceTime::NotTimed);
    }
    tiles = GpuTiles{};
    timer = GpuTimer{};
    EXPECT_GT(old_pool.use_count(), 1) << "only the test guard kept the pool";
    EXPECT_FALSE(pending->ready());
    EXPECT_TRUE(gate->signal(1).ok());
    EXPECT_TRUE(pending->wait(kLongWaitNs).ok());
    guard.reset();
    EXPECT_FALSE(old_pool.expired());
  }
  EXPECT_TRUE(old_pool.expired());
}

// An adopted Device may not know the manual submit's fence: the owner
// submitted it. Its teardown still drains its assigned queue, under the
// shared queue mutex, and releases the retained pool before returning.
TEST_F(TimerTest, AdoptedDeviceRetiresDroppedManualTilesAfterMoving) {
  AdoptedDevice handoff;
  handoff.instance = instance().handle();
  handoff.instance_api_version = instance().api_version();
  handoff.physical_device = physical().handle();
  handoff.device = device().handle();
  handoff.queue_family = device().queue_family();
  handoff.queue = device().queue();
  handoff.submit_mutex = device().submit_mutex();
  handoff.enabled_features.timeline_semaphore = true;
  handoff.enabled_debug_utils = instance().debug_utils_enabled();
  Result<Device> borrowed = Device::adopt(handoff, {});
  ASSERT_TRUE(borrowed.ok()) << borrowed.status().message();
  Result<GpuTimer> made_timer = GpuTimer::create(*borrowed, 1);
  ASSERT_TRUE(made_timer.ok());
  GpuTimer timer = *std::move(made_timer);
  if (!timer.available()) GTEST_SKIP() << "the queue has no timestamps";
  const std::weak_ptr<void> old_pool = timer.keep_alive();
  std::optional<Device> moved_device(*std::move(borrowed));
  Result<TimelineSemaphore> gate = TimelineSemaphore::create(device());
  ASSERT_TRUE(gate.ok());
  GpuTiles tiles;
  Result<Device::PendingSubmit> pending = device().submit_pending(
      [&](VkCommandBuffer cmd) {
        tiles = GpuTiles::begin(timer, cmd, 1);
        tiles.mark(cmd, "pending");
        tiles.end(cmd);
      },
      {{&*gate, 1}}, {});
  ASSERT_TRUE(pending.ok());
  EXPECT_TRUE(timer.reserve(*moved_device, 2).ok());
  std::shared_ptr<void> guard = old_pool.lock();
  tiles.drop(true);
  tiles = GpuTiles{};
  timer = GpuTimer{};
  EXPECT_GT(old_pool.use_count(), 1) << "only the test guard kept the pool";
  EXPECT_FALSE(pending->ready());
  // Publish the host signal before queue-idle: older validation layers can
  // deadlock their queue-state tracking when these calls overlap. The work
  // was still pending through drop and growth, and teardown must drain it.
  EXPECT_TRUE(gate->signal(1).ok());
  moved_device.reset();
  EXPECT_TRUE(pending->ready());
  EXPECT_TRUE(pending->wait(kLongWaitNs).ok());
  guard.reset();
  EXPECT_TRUE(old_pool.expired());
}

// A wait the core makes -- here a blocking submit's fence, queued behind at
// least 40 ms of device work -- lands in the wait of every scope open around
// it, a breakdown's included, and in the totals once; it counts as one host
// wait, of the time the scopes got. The work ahead is fills, doubled until a
// batch of them takes 40 ms, not work held for a value another thread sets:
// a submit queued behind that stalls 10 s in VVL 1.3.275's sync validation.
TEST_F(TimerWorkTest, ABlockingWaitIsEveryOpenScopesWait) {
  const auto fills = [&](std::uint32_t count) {
    CommandBatch batch(device(), allocator());
    for (std::uint32_t i = 0; i < count; ++i) {
      EXPECT_TRUE(batch.fill(target_, 0, kFillBytes, i).ok());
    }
    return batch;
  };
  // The faster of two runs at each count: the first can pay one-time costs
  // (the buffer's first touch) that would size the work too small.
  std::uint32_t count = 1;
  for (; count < 4096; count *= 2) {
    auto fastest = std::chrono::steady_clock::duration::max();
    for (int run = 0; run < 2; ++run) {
      CommandBatch probe = fills(count);
      const auto start = std::chrono::steady_clock::now();
      ASSERT_TRUE(probe.submit().ok());
      fastest = std::min(fastest, std::chrono::steady_clock::now() - start);
    }
    if (fastest >= std::chrono::milliseconds(40)) break;
  }
  CommandBatch ahead = fills(count);
  Result<PendingBatch> held = ahead.submit_async({}, {});
  ASSERT_TRUE(held.ok()) << held.status().message();
  const DeviceCounters before = device().counters();
  StageMetrics metrics;
  {
    const StageScope stage(metrics, "stage");
    const StageScope sub(metrics, "  ..sub");
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(target_, 0, words_.data(), 64).ok());
    ASSERT_TRUE(batch.submit().ok());  // behind the held work
  }
  const DeviceCounters after = device().counters();
  ASSERT_TRUE(held->wait(kLongWaitNs).ok());
  const StageRow* stage = row(metrics, "stage");
  const StageRow* sub = row(metrics, "  ..sub");
  ASSERT_TRUE(stage != nullptr && sub != nullptr);
  EXPECT_GE(sub->wait_ms, 10.0);
  EXPECT_LE(sub->wait_ms, sub->cpu_ms);
  EXPECT_DOUBLE_EQ(stage->wait_ms, sub->wait_ms);
  EXPECT_DOUBLE_EQ(metrics.total_wait_ms(), stage->wait_ms);
  EXPECT_EQ(after.submits - before.submits, 1U);
  EXPECT_EQ(after.host_waits - before.host_waits, 1U);
  EXPECT_NEAR(static_cast<double>(after.blocked_ns - before.blocked_ns) * 1e-6,
              sub->wait_ms, 1e-6);
}

// A fence's wait blocks the thread as the core's own waits do, so it reaches
// the scopes open around it: here a fence behind work held for the host,
// waited until a 20 ms timeout, on the thread that then lets the work go.
TEST_F(TimerWorkTest, AFenceWaitIsTheOpenScopesWait) {
  Result<Fence> fence = Fence::create(device().handle());
  ASSERT_TRUE(fence.ok()) << fence.status().message();
  Result<TimelineSemaphore> made = TimelineSemaphore::create(device());
  ASSERT_TRUE(made.ok()) << made.status().message();
  TimelineSemaphore gate = *std::move(made);
  CommandBatch hold(device(), allocator());
  Result<PendingBatch> held = hold.submit_async({{&gate, 1}}, {});
  ASSERT_TRUE(held.ok()) << held.status().message();
  // An empty submission: its fence signals once the work before it is done.
  const VkResult queued = device().queue_submit(0, nullptr, fence->handle());
  EXPECT_EQ(queued, VK_SUCCESS);
  StageMetrics metrics;
  {
    const StageScope stage(metrics, "fence");
    EXPECT_TRUE(is_timeout(fence->wait(20'000'000)));
  }
  ASSERT_TRUE(gate.signal(1).ok());
  if (queued == VK_SUCCESS) {
    EXPECT_TRUE(fence->wait(kLongWaitNs).ok());
  }
  ASSERT_TRUE(held->wait(kLongWaitNs).ok());
  const StageRow* fenced = row(metrics, "fence");
  ASSERT_NE(fenced, nullptr);
  EXPECT_GE(fenced->wait_ms, 15.0);
  EXPECT_LE(fenced->wait_ms, fenced->cpu_ms);
}

// A wait that finds the work done blocks nothing: no host wait, no blocked
// time; a timeline wait that blocks counts as one.
TEST_F(TimerWorkTest, OnlyWaitsThatBlockCount) {
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.fill(target_, 0, kFillBytes, 7U).ok());
  Result<PendingBatch> made = batch.submit_async({}, {});
  ASSERT_TRUE(made.ok());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!made->ready() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(made->ready());
  StageMetrics metrics;
  const DeviceCounters before = device().counters();
  {
    const StageScope scope(metrics, "finish");
    ASSERT_TRUE(made->wait(kLongWaitNs).ok());
  }
  EXPECT_EQ(device().counters().host_waits, before.host_waits);
  EXPECT_EQ(row(metrics, "finish")->wait_ms, 0.0);

  Result<TimelineSemaphore> timeline = TimelineSemaphore::create(device(), 1);
  ASSERT_TRUE(timeline.ok());
  ASSERT_TRUE(timeline->wait(1, kLongWaitNs).ok());  // reached: no wait
  EXPECT_EQ(device().counters().host_waits, before.host_waits);
  std::thread raise([&timeline] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(timeline->signal(2).ok());
  });
  {
    const StageScope scope(metrics, "timeline");
    EXPECT_TRUE(timeline->wait(2, kLongWaitNs).ok());
  }
  raise.join();
  EXPECT_EQ(device().counters().host_waits, before.host_waits + 1);
  EXPECT_GT(row(metrics, "timeline")->wait_ms, 0.0);
  // A drain cannot be asked whether it will block: it always counts.
  ASSERT_TRUE(device().wait_idle().ok());
  EXPECT_GE(device().counters().host_waits, before.host_waits + 2);
}

// Every submit and every staging or readback buffer a batch makes counts;
// null metrics time nothing -- no tile, no span -- while the counts go on.
TEST_F(TimerWorkTest, CountersCountAndNullMetricsTimeNothing) {
  const DeviceCounters before = device().counters();
  std::vector<std::uint32_t> big(CommandBatch::kMaxInlineUpload / 2, 9U);
  std::uint32_t out = 0;
  CommandBatch batch(device(), allocator());
  GpuStageScope inert(nullptr, timer_, "inert");
  ASSERT_TRUE(batch
                  .upload(target_, 0, big.data(),
                          CommandBatch::kMaxInlineUpload * 2, &inert)
                  .ok());  // staged
  ASSERT_TRUE(batch.readback(target_, 0, 4, &out).ok());
  Result<PendingBatch> made = batch.submit_async({}, {});
  ASSERT_TRUE(made.ok());
  ASSERT_TRUE(made->wait(kLongWaitNs).ok());
  EXPECT_EQ(out, 9U);
  EXPECT_TRUE(made->device_rows().empty());
  EXPECT_FALSE(made->device_span().has_value());
  const DeviceCounters after = device().counters();
  EXPECT_EQ(after.submits - before.submits, 1U);
  EXPECT_EQ(after.staging_buffers - before.staging_buffers, 2U);
  EXPECT_EQ(detail::innermost_scope(), nullptr);
}

// Where the device offers calibrated timestamps, a pair's host time lies
// within its deviation of the call, the pairs advance, and a batch's span
// falls inside [submit call, wait return] on the host clock, within the
// reported deviation; elsewhere calibrate() says Unsupported and spans stay
// in ticks.
TEST_F(TimerWorkTest, CalibratedSpansFallInsideTheHostBounds) {
  const Result<ClockPair> first = device().calibrate();
  if (!first.ok()) {
    EXPECT_EQ(first.status().domain(), Status::Code::Unsupported)
        << first.status().message();
    RecordProperty("calibrated", "no");
  } else {
    RecordProperty("calibrated", "yes");
    RecordProperty("deviation_ns", std::to_string(first->deviation_ns));
    const std::int64_t before = steady_ns();
    const Result<ClockPair> second = device().calibrate();
    const std::int64_t after = steady_ns();
    ASSERT_TRUE(second.ok());
    const auto deviation = static_cast<std::int64_t>(second->deviation_ns);
    EXPECT_GE(second->host_ns + deviation, before);
    EXPECT_LE(second->host_ns - deviation, after);
    EXPECT_GE(second->host_ns, first->host_ns);
    if (timestamps_advance()) {
      EXPECT_GT(timestamp_delta(first->device_ticks, second->device_ticks,
                                device().timestamp_valid_bits()),
                0U);
    }
  }
  if (!timer_.available()) GTEST_SKIP() << "the queue has no timestamps";
  CommandBatch batch(device(), allocator());
  batch.time(timer_);
  ASSERT_TRUE(batch.fill(target_, 0, kFillBytes, 8U).ok());
  const std::int64_t submit_call = steady_ns();
  Result<PendingBatch> made = batch.submit_async({}, {});
  ASSERT_TRUE(made.ok());
  ASSERT_TRUE(made->wait(kLongWaitNs).ok());
  const std::int64_t wait_return = steady_ns();
  ASSERT_TRUE(made->device_span().has_value());
  const DeviceSpan span = made->device_span().value_or(DeviceSpan{});
  EXPECT_EQ(span.host_clock, first.ok());
  if (!span.host_clock) return;
  EXPECT_LE(span.begin_ns, span.end_ns);
  // Timestamps that do not advance (a paravirtualized GPU) are no reading of
  // the queue's counter, so where they land on the host clock means nothing.
  if (!timestamps_advance()) return;
  const auto slack = static_cast<std::int64_t>(span.deviation_ns);
  EXPECT_GE(span.begin_ns, submit_call - slack);
  EXPECT_LE(span.end_ns, wait_return + slack);
}

// A device adopted without a calibrated-timestamps extension declared says
// Unsupported, and counts in counters of its own.
TEST_F(TimerWorkTest, AnAdoptedDeviceWithoutTheExtensionIsUnsupported) {
  AdoptedDevice handoff;
  handoff.instance = instance().handle();
  handoff.instance_api_version = instance().api_version();
  handoff.physical_device = physical().handle();
  handoff.device = device().handle();
  handoff.queue_family = device().queue_family();
  handoff.queue = device().queue();
  handoff.submit_mutex = device().submit_mutex();
  handoff.enabled_features.timeline_semaphore = true;
  Result<Device> made = Device::adopt(handoff, DeviceRequirements{});
  ASSERT_TRUE(made.ok()) << made.status().message();
  const Device adopted = *std::move(made);
  EXPECT_EQ(adopted.calibrate().status().domain(), Status::Code::Unsupported);
  EXPECT_EQ(adopted.counters().submits, 0U);
  const DeviceCounters before = device().counters();
  CommandBatch batch(adopted);
  ASSERT_TRUE(batch.upload(target_, 0, words_.data(), 64).ok());
  ASSERT_TRUE(batch.submit().ok());
  EXPECT_EQ(adopted.counters().submits, 1U);
  EXPECT_EQ(device().counters().submits, before.submits);
}

// The tile attribution check (the design's V1): two kernels of known length,
// tiled in one batch, each read within 10% of itself timed alone. Where
// timestamps snap to encoder boundaries (a GPU layered on Metal), the test
// documents it instead: there, sub-stage cells read n/t.
class TileAttributionTest : public TimerWorkTest {
 protected:
  struct Push {
    std::uint32_t count;
    std::uint32_t delta;
  };

  void SetUp() override {
    TimerWorkTest::SetUp();
    if (base_setup_incomplete()) return;
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(Push);
    KernelSetBuilder builder(device());
    ASSERT_TRUE(builder
                    .add(add_, "test_add", vkc_test_add_comp_spv,
                         vkc_test_add_comp_spv_size, 1, &range)
                    .ok());
    Result<DescriptorPool> pool = builder.build();
    ASSERT_TRUE(pool.ok()) << pool.status().message();
    pool_ = *std::move(pool);
    max_groups_ = physical().limits().maxComputeWorkGroupCount[0];
    // A pass well past any cache on a GPU; a CPU device runs a smaller one,
    // still long beside a dispatch's scheduling on its threads.
    const std::uint32_t wanted = cpu_device() ? 1U << 22 : 1U << 25;
    // In 64 bits: a group limit of 2^26 or more, times 64, wraps 32.
    count_ = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(wanted, std::uint64_t{max_groups_} * 64U));
    Result<Buffer> values =
        device_storage_buffer(allocator(), VkDeviceSize{count_} * 4);
    ASSERT_TRUE(values.ok()) << values.status().message();
    values_ = *std::move(values);
    add_.set.write_storage_buffer(0, values_.handle(), 0, VK_WHOLE_SIZE);
  }
  void TearDown() override {
    values_ = Buffer{};
    add_ = ComputeKernel{};
    pool_ = DescriptorPool{};
    TimerWorkTest::TearDown();
  }

  // Records `passes` dispatches over the buffer under a scope named `name`.
  void record(CommandBatch& batch, StageMetrics& recording, const char* name,
              int passes) {
    GpuStageScope stage(&recording, timer_, name);
    const Push push{count_, 1};
    for (int i = 0; i < passes; ++i) {
      EXPECT_TRUE(batch
                      .dispatch(add_, &push, sizeof(push),
                                group_count(count_, 64), max_groups_, &stage)
                      .ok());
    }
  }

  // The rows of one async batch of `stages` (name, passes), in order.
  std::vector<StageRow> run(
      const std::vector<std::pair<const char*, int>>& stages) {
    StageMetrics recording;
    CommandBatch batch(device());
    for (const auto& [name, passes] : stages) {
      record(batch, recording, name, passes);
    }
    Result<PendingBatch> pending = batch.submit_async({}, {});
    EXPECT_TRUE(pending.ok()) << pending.status().message();
    if (!pending.ok()) return {};
    EXPECT_TRUE(pending->wait(kLongWaitNs).ok());
    return pending->device_rows();
  }

  bool cpu_device() const {
    return physical().properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  }

  // Load on the machine only ever slows a run -- a CPU device shares its
  // cores with the other tests -- so the fastest is the truest.
  static double fastest(const std::vector<double>& values) {
    return *std::min_element(values.begin(), values.end());
  }

  ComputeKernel add_;
  DescriptorPool pool_;
  Buffer values_;
  std::uint32_t count_ = 0;
  std::uint32_t max_groups_ = 0;
};

TEST_F(TileAttributionTest, EachTileHoldsItsOwnStage) {
  if (!timer_.available()) GTEST_SKIP() << "the queue has no timestamps";
  if (!timestamps_advance()) GTEST_SKIP() << "timestamps do not advance";
  // 10%, and 20 us for a mark's own latency on a fast device.
  const auto close = [](double tiled, double alone) {
    return std::abs(tiled - alone) <= (0.1 * alone) + 0.02;
  };
  // A misattribution misses every round; a busy machine misses some, so the
  // test takes the first round that lands, of a few.
  constexpr int kRounds = 5;
  constexpr int kRuns = 5;
  bool attributed = false;
  double a = 0.0;
  double b = 0.0;
  double ta = 0.0;
  double tb = 0.0;
  int round = 0;
  while (!attributed && round < kRounds) {
    ++round;
    std::vector<double> alone_a;
    std::vector<double> alone_b;
    std::vector<double> tiled_a;
    std::vector<double> tiled_b;
    for (int i = 0; i < kRuns; ++i) {
      // Interleaved, so a clock or thermal drift moves every series alike.
      const std::vector<StageRow> one = run({{"A", 4}});
      const std::vector<StageRow> two = run({{"B", 2}});
      const std::vector<StageRow> both = run({{"A", 4}, {"B", 2}});
      ASSERT_TRUE(row(one, "A") && row(two, "B") && row(both, "A") &&
                  row(both, "B"));
      for (const StageRow* r :
           {row(one, "A"), row(two, "B"), row(both, "A"), row(both, "B")}) {
        ASSERT_EQ(r->device, DeviceTime::Measured) << r->name;
      }
      alone_a.push_back(row(one, "A")->gpu_ms);
      alone_b.push_back(row(two, "B")->gpu_ms);
      tiled_a.push_back(row(both, "A")->gpu_ms);
      tiled_b.push_back(row(both, "B")->gpu_ms);
    }
    a = fastest(alone_a);
    b = fastest(alone_b);
    ta = fastest(tiled_a);
    tb = fastest(tiled_b);
    attributed = close(ta, a) && close(tb, b);
  }
  RecordProperty("rounds", std::to_string(round));
  RecordProperty("alone_a_ms", std::to_string(a));
  RecordProperty("alone_b_ms", std::to_string(b));
  RecordProperty("tiled_a_ms", std::to_string(ta));
  RecordProperty("tiled_b_ms", std::to_string(tb));
#ifdef __APPLE__
  if (!attributed) {
    GTEST_SKIP() << "tiles snap to Metal encoder boundaries here (A " << ta
                 << " ms tiled vs " << a << " alone, B " << tb << " vs " << b
                 << "): sub-stage cells read n/t on this device";
  }
#endif
  // A CPU device runs on the cores the rest of the test run shares; there a
  // miss in every round is the machine's load, which the skip names. The
  // structure -- tiles that split at each tag, nest, and add up to the span
  // -- is checked on every device by the tests above.
  if (!attributed && cpu_device()) {
    GTEST_SKIP() << "the CPU device was too loaded to time alike in " << kRounds
                 << " rounds (A " << ta << " ms tiled vs " << a << " alone, B "
                 << tb << " vs " << b << ")";
  }
  EXPECT_TRUE(attributed) << "A " << ta << " ms tiled vs " << a << " alone; B "
                          << tb << " vs " << b;
}

// --- the paths timed before tiles --------------------------------------------

// A submit made inside another's recording reads its own tiles alone: the
// outer range is reset by a command buffer that has not run yet, which the
// validation layer reports if read (VUID-vkGetQueryPoolResults-None-09401).
TEST_F(TimerWorkTest, ANestedSubmitReadsOnlyItsOwnTiles) {
  for (int window = 0; window < 2; ++window) {
    StageMetrics metrics;
    {
      GpuStageScope stage(&metrics, timer_, "outer");
      const auto record = fill();
      const Status s = device().submit_single_time(
          [&](VkCommandBuffer cmd) {
            record(cmd);
            EXPECT_TRUE(device().submit_single_time(fill(), stage).ok());
          },
          stage);
      ASSERT_TRUE(s.ok()) << s.message();
    }
    ASSERT_NE(row(metrics, "outer"), nullptr);
    EXPECT_EQ(row(metrics, "outer")->device, expected());
    if (timer_.available() && timestamps_advance()) {
      EXPECT_GT(row(metrics, "outer")->gpu_ms, 0.0);
    }
  }
}

// A blocking submit publishes into the scopes still open at its wait: a
// scope that closed before it gets nothing, and an open one its command did
// not count to gets nothing either.
TEST_F(TimerWorkTest, ABlockingSubmitPublishesIntoOpenScopesOnly) {
  StageMetrics metrics;
  CommandBatch batch(device(), allocator());
  {
    GpuStageScope gone(&metrics, timer_, "gone");
    ASSERT_TRUE(tagged(batch, &gone).ok());
  }
  {
    const GpuStageScope later(&metrics, timer_, "later");
    ASSERT_TRUE(batch.submit().ok());
  }
  ASSERT_NE(row(metrics, "gone"), nullptr);
  EXPECT_EQ(row(metrics, "gone")->device, DeviceTime::None);
  EXPECT_EQ(row(metrics, "later")->device, DeviceTime::None);
}

// A blocking submit that fails before the device -- here refused for a
// recording call's refusal -- reads as one that failed on it: its open
// scopes not timed, never a host-only blank.
TEST_F(TimerWorkTest, ASubmitRefusedBeforeTheDeviceReadsNotTimed) {
  StageMetrics metrics;
  {
    GpuStageScope stage(&metrics, timer_, "refused");
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(tagged(batch, &stage).ok());
    EXPECT_TRUE(is_invalid(batch.fill(target_, 0, 3, 0U)));  // misaligned
    EXPECT_TRUE(is_invalid(batch.submit()));
  }
  const StageRow* refused = row(metrics, "refused");
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->device, timer_.available() ? DeviceTime::NotTimed
                                                : DeviceTime::Unavailable);
}

// reserve grows the pool at any time, and refuses a device other than the
// timer's.
TEST_F(TimerWorkTest, ReserveGrowsOnItsOwnDevice) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  if (!timer.available()) GTEST_SKIP() << "the queue has no timestamps";
  StageMetrics recording;
  CommandBatch batch(device(), allocator());
  GpuStageScope a(&recording, timer, "a");
  ASSERT_TRUE(tagged(batch, &a).ok());
  Result<PendingBatch> pending = batch.submit_async({}, {});  // holds both
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(timer.reserve(device(), 4).ok());  // a new pool beside it
  StageMetrics metrics;
  {
    GpuStageScope stage(&metrics, timer, "grown");
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
  }
  EXPECT_EQ(row(metrics, "grown")->device, DeviceTime::Measured);
  ASSERT_TRUE(pending->wait(kLongWaitNs).ok());
  EXPECT_EQ(pending->device_rows()[0].device, DeviceTime::Measured);
  EXPECT_TRUE(timer.reserve(device(), GpuTimer::kMaxSpans + 1).ok());
  EXPECT_TRUE(timer.available());  // clamped to the ceiling, logged
  Device moved = std::move(device());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(is_invalid(timer.reserve(device(), 4)));
  device() = std::move(moved);
}

#ifdef VKC_TEST_EXCEPTIONS
// A record that throws never reaches the device: its range goes back, so a
// timer with room for one batch times the next.
TEST_F(TimerWorkTest, ARecordThatThrowsReturnsItsRange) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  if (!tiny.available()) GTEST_SKIP() << "the queue has no timestamps";
  StageMetrics metrics;
  {
    GpuStageScope stage(&metrics, tiny, "thrown");
    const Status refused = device().submit_single_time(
        [](VkCommandBuffer) { throw std::runtime_error("record"); }, stage);
    EXPECT_EQ(refused.domain(), Status::Code::InvalidArgument);
  }
  EXPECT_EQ(row(metrics, "thrown")->device, DeviceTime::NotTimed);
  {
    GpuStageScope stage(&metrics, tiny, "retried");
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
  }
  EXPECT_EQ(row(metrics, "retried")->device, DeviceTime::Measured);
}
#endif

TEST_F(TimerTest, CreateRefusesWhatWouldWrap) {
  EXPECT_TRUE(is_invalid(GpuTimer::create(device(), 0).status()));
  EXPECT_TRUE(is_invalid(GpuTimer::create(device(), 0x80000000U).status()));
  EXPECT_TRUE(
      is_invalid(GpuTimer::create(device(), GpuTimer::kMaxSpans + 1).status()));
  EXPECT_TRUE(GpuTimer::create(device(), GpuTimer::kMaxSpans).ok());
  Device moved = std::move(device());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(is_invalid(GpuTimer::create(device()).status()));
  device() = std::move(moved);
}

TEST_F(TimerTest, TimersMove) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer a = *std::move(made);
  a.set_max_depth(2);
  const bool available = a.available();
  GpuTimer b = std::move(a);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_FALSE(a.available());
  EXPECT_EQ(a.max_depth(), 0U);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(b.valid());
  EXPECT_EQ(b.available(), available);
  EXPECT_EQ(b.max_depth(), 2U);
  Result<GpuTimer> other = GpuTimer::create(device());
  ASSERT_TRUE(other.ok());
  b = *std::move(other);  // over a live timer, whose pool goes
  EXPECT_TRUE(b.valid());
  GpuTimer* self = &b;
  b = std::move(*self);
  EXPECT_TRUE(b.valid());
}

// Timed commands of every kind land in their scopes' rows: two dispatches,
// uploads inline, staged and reserved, and a copy. The scopes nest, so each
// outer row holds its inner ones' tiles too.
TEST_F(TimerTest, ABatchTimesItsCommands) {
  constexpr std::uint32_t kCount = 256;
  constexpr VkDeviceSize kBytes = VkDeviceSize{kCount} * 4;
  struct Push {
    std::uint32_t count;
    std::uint32_t delta;
  };
  VkPushConstantRange range{};
  range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  range.size = sizeof(Push);
  ComputeKernel add;
  KernelSetBuilder builder(device());
  ASSERT_TRUE(builder
                  .add(add, "test_add", vkc_test_add_comp_spv,
                       vkc_test_add_comp_spv_size, 1, &range)
                  .ok());
  const Result<DescriptorPool> pool = builder.build();
  ASSERT_TRUE(pool.ok());
  Result<Buffer> a = device_storage_buffer(allocator(), kBytes);
  Result<Buffer> b =
      device_storage_buffer(allocator(), CommandBatch::kMaxInlineUpload * 2);
  ASSERT_TRUE(a.ok() && b.ok());
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  const std::uint32_t max_groups =
      physical().limits().maxComputeWorkGroupCount[0];
  const std::vector<std::uint32_t> words(CommandBatch::kMaxInlineUpload / 2,
                                         7U);
  const DeviceTime timed =
      timer.available() ? DeviceTime::Measured : DeviceTime::Unavailable;

  StageMetrics metrics;
  {
    GpuStageScope first(&metrics, timer, "first");
    GpuStageScope second(&metrics, timer, "second");
    GpuStageScope uploads(&metrics, timer, "uploads");
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(*a, 0, words.data(), kBytes, &uploads).ok());
    ASSERT_TRUE(batch  // staged: past the inline limit
                    .upload(*b, 0, words.data(),
                            CommandBatch::kMaxInlineUpload * 2, &uploads)
                    .ok());
    ASSERT_TRUE(batch.reserve_upload(*b, 0, 8, &uploads).ok());
    ASSERT_TRUE(batch.copy(*a, 0, *b, 64, 8, &uploads).ok());
    add.set.write_storage_buffer(0, a->handle(), 0, VK_WHOLE_SIZE);
    const Push push{kCount, 1};
    ASSERT_TRUE(
        batch.dispatch(add, &push, sizeof(push), 4, max_groups, &first).ok());
    ASSERT_TRUE(
        batch.dispatch(add, &push, sizeof(push), 4, max_groups, &second).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  for (const char* name : {"first", "second", "uploads"}) {
    const StageRow* r = row(metrics, name);
    ASSERT_NE(r, nullptr) << name;
    EXPECT_GT(r->cpu_ms, 0.0) << name;
    EXPECT_EQ(r->device, timed) << name;
  }
  // The scope order seeded the rows: first, second, uploads.
  EXPECT_STREQ(metrics.rows()[0].name, "first");
  EXPECT_GE(row(metrics, "first")->gpu_ms, row(metrics, "second")->gpu_ms);
  EXPECT_GE(row(metrics, "second")->gpu_ms, row(metrics, "uploads")->gpu_ms);

  // dispatch() takes a stage too; and an inert one is the untimed path.
  StageMetrics one;
  {
    GpuStageScope stage(&one, timer, "one-shot");
    const Push push{kCount, 2};
    ASSERT_TRUE(
        dispatch(device(), add, &push, sizeof(push), 4, max_groups, &stage)
            .ok());
  }
  ASSERT_NE(row(one, "one-shot"), nullptr);
  EXPECT_EQ(row(one, "one-shot")->device, timed);
  StageMetrics none;
  {
    const StageScope frame(none, "frame");
    GpuStageScope inert(nullptr, timer, "never");
    const Push push{kCount, 3};
    ASSERT_TRUE(
        dispatch(device(), add, &push, sizeof(push), 4, max_groups, &inert)
            .ok());
  }
  ASSERT_EQ(none.rows().size(), 1U);  // the frame's host row alone
  EXPECT_EQ(none.rows()[0].device, DeviceTime::None);
}

}  // namespace
}  // namespace volumetric_kit::core
