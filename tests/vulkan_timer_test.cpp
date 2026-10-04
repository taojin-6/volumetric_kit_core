// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// QueryPool, GpuTimer and GpuStageScope, and the spans CommandBatch,
// dispatch() and the Device overload record, ported from recon's
// core_stage_metrics_test and core_command_batch_test. The tick math and
// argument checks need no device.

#include "volumetric_kit/core/vulkan/gpu_timer.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "add_comp.spv.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
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
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "vulkan_device_fixture.hpp"

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
const StageRow* row(const StageMetrics& metrics, const char* name) {
  for (const StageRow& r : metrics.rows()) {
    if (std::string(r.name) == name) return &r;
  }
  return nullptr;
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

TEST(QueryPoolArgs, RefusesANullDeviceOrNoQueries) {
  EXPECT_TRUE(is_invalid(QueryPool::create(VK_NULL_HANDLE, 2).status()));
  const QueryPool empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.query_count(), 0U);
  std::uint64_t out[2] = {};
  EXPECT_TRUE(is_invalid(empty.read_results(0, 1, out)));
}

TEST(GpuTimerEmpty, IsInert) {
  GpuTimer timer;
  EXPECT_FALSE(timer.valid());
  EXPECT_FALSE(timer.available());
  EXPECT_EQ(timer.begin(VK_NULL_HANDLE, "x"), GpuTimer::kNoSpan);
  timer.end(VK_NULL_HANDLE, GpuTimer::kNoSpan);
  EXPECT_TRUE(timer.resolve(0).ok());
  timer.settle(0, 1, Status{}, /*in_flight=*/false);
  StageMetrics metrics;
  timer.report_into(metrics);
  EXPECT_TRUE(metrics.empty());
  EXPECT_EQ(timer.keep_alive(), nullptr);
}

// A span's name lands in a StageMetrics row, which a null one would crash.
TEST(GpuTimerDeathTest, ANullNameIsAProgrammerError) {
  GpuTimer timer;
  EXPECT_DEATH(timer.begin(VK_NULL_HANDLE, nullptr), "name is null");
}

TEST(GpuStageScopeInert, NullMetricsTimesNothing) {
  GpuTimer timer;
  {
    const GpuStageScope stage(nullptr, timer, "never");
    EXPECT_EQ(stage.timer(), nullptr);
    EXPECT_STREQ(stage.name(), "never");
  }
  StageMetrics metrics;
  {
    const GpuStageScope stage(&metrics, timer, "host only");
    EXPECT_EQ(stage.timer(), &timer);
  }
  // An empty timer adds no device row; the host row still lands.
  ASSERT_EQ(metrics.rows().size(), 1U);
  EXPECT_FALSE(metrics.rows()[0].has_gpu);
}

// --- on a device
// --------------------------------------------------------------

// Large enough that filling it is clearly more than a tick on any GPU the
// family targets, so a device span above zero means something.
constexpr VkDeviceSize kFillBytes = VkDeviceSize{64} << 20;

class TimerTest : public test::VulkanDeviceTest {
 protected:
  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    Result<Buffer> made = device_storage_buffer(allocator(), kFillBytes);
    ASSERT_TRUE(made.ok()) << made.status().message();
    target_ = *std::move(made);
  }
  void TearDown() override {
    target_ = Buffer{};
    VulkanDeviceTest::TearDown();
  }

  // Whether the device's timestamps advance across a fill. A paravirtualized
  // GPU (GitHub's macOS runners) reports valid bits and returns one value for
  // every timestamp, so the magnitude of a span is checked only where they
  // move; that spans are recorded, resolved and published is checked
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

  Buffer target_;
  std::optional<bool> advance_;
};

// Says, as a skip, when this device's timestamps do not advance -- why the
// tests below check no span's magnitude here.
TEST_F(TimerTest, TimestampsAdvance) {
  if (device().timestamp_valid_bits() == 0) {
    GTEST_SKIP() << "the queue family has no timestamps";
  }
  if (!timestamps_advance()) {
    GTEST_SKIP() << "timestamps do not advance on this device (a "
                    "paravirtualized GPU): span magnitudes go unchecked";
  }
}

TEST_F(TimerTest, QueryPoolWritesAndReadsTimestamps) {
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

// The device span is the recorded work alone, so it comes in under the wall
// clock around the whole blocking submit -- if it were the wall clock
// relabelled, or resolved against the wrong period, it would not.
TEST_F(TimerTest, TimesTheWorkNotTheSubmit) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok()) << made.status().message();
  GpuTimer timer = *std::move(made);
  EXPECT_TRUE(timer.valid());
  StageMetrics metrics;
  double wall_ms = 0.0;
  {
    GpuStageScope stage(&metrics, timer, "fill");
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
  if (!timer.available()) {
    // A family without timestamps is supported: no device row, not a zero.
    EXPECT_FALSE(fill_row->has_gpu);
    GTEST_SKIP() << "the queue family has no timestamps";
  }
  ASSERT_TRUE(fill_row->has_gpu);
  if (timestamps_advance()) {
    EXPECT_GT(fill_row->gpu_ms, 0.0);
    EXPECT_LT(fill_row->gpu_ms, wall_ms);
  }
  EXPECT_EQ(timer.count(), 0U);  // publishing ended the window
}

TEST_F(TimerTest, AWindowIsBoundedPublishedOnceAndReusable) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  if (!tiny.available()) GTEST_SKIP() << "the queue family has no timestamps";

  // A one-span window refuses the second span -- through real submits, so the
  // bound itself is reached -- and the submit still succeeds.
  const auto timed = [&](const char* name) {
    std::uint32_t span = GpuTimer::kNoSpan;
    const auto record = fill();
    const Status s = device().submit_single_time([&](VkCommandBuffer cmd) {
      span = tiny.begin(cmd, name);
      record(cmd);
      tiny.end(cmd, span);
    });
    EXPECT_TRUE(s.ok()) << s.message();
    if (span != GpuTimer::kNoSpan) {
      EXPECT_TRUE(tiny.resolve(span).ok());
    }
    return span;
  };
  EXPECT_NE(timed("a"), GpuTimer::kNoSpan);
  EXPECT_EQ(tiny.count(), 1U);
  EXPECT_EQ(timed("b"), GpuTimer::kNoSpan);  // full
  EXPECT_EQ(tiny.count(), 1U);

  // Each window reports its own spans, then the timer starts empty.
  StageMetrics first;
  tiny.report_into(first);
  ASSERT_EQ(first.rows().size(), 1U);
  EXPECT_STREQ(first.rows()[0].name, "a");
  EXPECT_EQ(tiny.count(), 0U);
  EXPECT_NE(timed("c"), GpuTimer::kNoSpan);
  StageMetrics second;
  tiny.report_into(second);
  ASSERT_EQ(second.rows().size(), 1U);
  EXPECT_STREQ(second.rows()[0].name, "c");
  EXPECT_TRUE(second.rows()[0].has_gpu);
  if (timestamps_advance()) {
    EXPECT_GT(second.rows()[0].gpu_ms, 0.0);
  }
  StageMetrics republished;
  tiny.report_into(republished);  // an ended window adds nothing
  EXPECT_TRUE(republished.empty());

  // reserve raises the bound between windows, not within one.
  timed("d");
  ASSERT_TRUE(tiny.reserve(device(), 2).ok());
  EXPECT_EQ(timed("e"), GpuTimer::kNoSpan);
  tiny.reset();
  ASSERT_TRUE(tiny.reserve(device(), 2).ok());
  EXPECT_NE(timed("f"), GpuTimer::kNoSpan);
  EXPECT_NE(timed("g"), GpuTimer::kNoSpan);
  EXPECT_EQ(tiny.count(), 2U);
  tiny.reset();
  EXPECT_EQ(tiny.count(), 0U);
}

// Recorded and never submitted, as by a submit that failed before the device:
// the spans settle by being dropped, newest run only, and the timer keeps
// timing; work the device may still run retires it.
TEST_F(TimerTest, SettleDropsWhatNeverRanAndRetiresWhatMayStillRun) {
  Result<GpuTimer> made = GpuTimer::create(device(), 4);
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  if (!timer.available()) GTEST_SKIP() << "the queue family has no timestamps";
  Result<CommandPool> pool =
      CommandPool::create(device().handle(), device().queue_family());
  ASSERT_TRUE(pool.ok());
  Result<CommandBuffer> cmd = pool->allocate_primary();
  ASSERT_TRUE(cmd.ok());
  ASSERT_TRUE(cmd->begin().ok());
  const auto timed = [&](const char* name) {
    const std::uint32_t span = timer.begin(cmd->handle(), name);
    timer.end(cmd->handle(), span);
    return span;
  };
  const std::uint32_t first = timed("first");
  const std::uint32_t second = timed("second");
  const std::uint32_t third = timed("third");
  ASSERT_TRUE(cmd->end().ok());
  EXPECT_EQ(timer.count(), 3U);
  const Status refused = vk_error(VK_ERROR_OUT_OF_HOST_MEMORY, "vkQueueSubmit");

  timer.discard(first);  // not the newest: kept
  timer.discard(second, 1);
  timer.discard(GpuTimer::kNoSpan);
  EXPECT_EQ(timer.count(), 3U);
  timer.settle(third, 1, refused, /*in_flight=*/false);
  EXPECT_EQ(timer.count(), 2U);
  timer.settle(first, 2, refused, /*in_flight=*/false);  // the newest run now
  EXPECT_EQ(timer.count(), 0U);
  EXPECT_TRUE(timer.available());
  // Nothing unsubmitted is left to read: resolving it would read queries no
  // command buffer reset (VUID-vkGetQueryPoolResults-None-09401).
  StageMetrics metrics;
  timer.report_into(metrics);
  EXPECT_TRUE(metrics.empty());

  // The device may still run it: the queries cannot be reused, ever.
  const std::shared_ptr<void> queries = timer.keep_alive();
  timer.settle(0, 0, refused, /*in_flight=*/true);
  EXPECT_FALSE(timer.available());
  EXPECT_EQ(timer.keep_alive(), queries);  // held, for the device to keep
}

// What the window resolved before the timer retired is still published;
// nothing is timed after.
TEST_F(TimerTest, AbandonRetiresTheTimer) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  if (!timer.available()) GTEST_SKIP() << "the queue family has no timestamps";
  StageMetrics metrics;
  {
    GpuStageScope stage(&metrics, timer, "before");
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
    timer.abandon();
  }
  ASSERT_NE(row(metrics, "before"), nullptr);
  EXPECT_TRUE(row(metrics, "before")->has_gpu);
  EXPECT_TRUE(timer.valid());
  EXPECT_FALSE(timer.available());
  {
    GpuStageScope stage(&metrics, timer, "after");
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
  }
  ASSERT_NE(row(metrics, "after"), nullptr);
  EXPECT_FALSE(row(metrics, "after")->has_gpu);
}

// A submit made inside another's recording reads its own span alone: the
// outer span's queries are reset by a command buffer that has not run yet,
// which the validation layer reports if read (VUID-vkGetQueryPoolResults-
// None-09401), and in a later window hold the previous frame's value.
TEST_F(TimerTest, ANestedSubmitReadsOnlyItsOwnSpan) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  if (!timer.available()) GTEST_SKIP() << "the queue family has no timestamps";
  for (int window = 0; window < 2; ++window) {
    StageMetrics metrics;
    {
      GpuStageScope stage(&metrics, timer, "outer");
      const auto record = fill();
      const Status s = device().submit_single_time(
          [&](VkCommandBuffer cmd) {
            record(cmd);
            EXPECT_TRUE(device().submit_single_time(fill(), stage).ok());
          },
          stage);
      ASSERT_TRUE(s.ok()) << s.message();
      EXPECT_EQ(timer.count(), 2U);
    }
    ASSERT_NE(row(metrics, "outer"), nullptr);
    EXPECT_TRUE(row(metrics, "outer")->has_gpu);
    if (timestamps_advance()) {
      EXPECT_GT(row(metrics, "outer")->gpu_ms, 0.0);
    }
  }
}

// Two scopes open on one timer, with metrics of their own -- a stage, and a
// helper inside it: each publishes its own spans, and the window ends with
// the outer one.
TEST_F(TimerTest, EachScopePublishesOnlyItsOwnSpans) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  StageMetrics frame;
  StageMetrics local;
  {
    GpuStageScope stage(&frame, timer, "integrate");
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
    {
      GpuStageScope helper(&local, timer, "sub");
      ASSERT_TRUE(device().submit_single_time(fill(), helper).ok());
      ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
    }
    EXPECT_EQ(row(local, "integrate"), nullptr);
    if (timer.available()) {
      EXPECT_EQ(timer.count(), 3U);  // the stage's two are still to publish
    }
  }
  EXPECT_EQ(timer.count(), 0U);
  EXPECT_EQ(row(frame, "sub"), nullptr);
  ASSERT_NE(row(frame, "integrate"), nullptr);
  ASSERT_NE(row(local, "sub"), nullptr);
  EXPECT_EQ(row(frame, "integrate")->has_gpu, timer.available());
  EXPECT_EQ(row(local, "sub")->has_gpu, timer.available());
}

// A batch keeps the scope's tag, not the scope: one that closes before the
// submit leaves the command untimed rather than dangling.
TEST_F(TimerTest, AScopeClosedBeforeItsSubmitLeavesTheCommandUntimed) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  const std::vector<std::uint32_t> words(16, 7U);
  StageMetrics metrics;
  CommandBatch batch(device(), allocator());
  {
    GpuStageScope gone(&metrics, timer, "gone");
    ASSERT_TRUE(batch.upload(target_, 0, words.data(), 64, &gone).ok());
  }
  {
    const GpuStageScope later(&metrics, timer, "later");
    ASSERT_TRUE(batch.submit().ok());
    EXPECT_EQ(timer.count(), 0U);
  }
  ASSERT_NE(row(metrics, "gone"), nullptr);
  EXPECT_FALSE(row(metrics, "gone")->has_gpu);
  EXPECT_FALSE(row(metrics, "later")->has_gpu);
}

// A stage that loses a span to a full window publishes no device time, not
// the part that fit; the next window times it whole.
TEST_F(TimerTest, AStageThatFillsTheWindowReportsNoDeviceTime) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made.ok());
  GpuTimer tiny = *std::move(made);
  if (!tiny.available()) GTEST_SKIP() << "the queue family has no timestamps";
  const std::vector<std::uint32_t> words(16, 7U);
  StageMetrics full;
  {
    GpuStageScope stage(&full, tiny, "big");
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(target_, 0, words.data(), 64, &stage).ok());
    ASSERT_TRUE(batch.upload(target_, 64, words.data(), 64, &stage).ok());
    ASSERT_TRUE(batch.submit().ok());
    EXPECT_EQ(tiny.count(), 1U);
  }
  ASSERT_NE(row(full, "big"), nullptr);
  EXPECT_FALSE(row(full, "big")->has_gpu);
  StageMetrics fits;
  {
    GpuStageScope stage(&fits, tiny, "big");
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.upload(target_, 0, words.data(), 64, &stage).ok());
    ASSERT_TRUE(batch.submit().ok());
  }
  EXPECT_TRUE(row(fits, "big")->has_gpu);
}

// reserve grows the pool inside an open scope, which keeps publishing, and
// refuses a device other than the timer's.
TEST_F(TimerTest, ReserveGrowsWithinAScopeOnItsOwnDevice) {
  Result<GpuTimer> made = GpuTimer::create(device(), 1);
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  if (!timer.available()) GTEST_SKIP() << "the queue family has no timestamps";
  StageMetrics metrics;
  {
    GpuStageScope stage(&metrics, timer, "grown");
    ASSERT_TRUE(timer.reserve(device(), 2).ok());
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
    EXPECT_EQ(timer.count(), 2U);
  }
  EXPECT_TRUE(row(metrics, "grown")->has_gpu);
  EXPECT_TRUE(timer.reserve(device(), GpuTimer::kMaxSpans + 1).ok());
  EXPECT_TRUE(timer.available());  // clamped to the ceiling, logged
  Device moved = std::move(device());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(is_invalid(timer.reserve(device(), 4)));
  device() = std::move(moved);
}

#ifdef VKC_TEST_EXCEPTIONS
// A record that throws never reaches the device: its span is dropped, and
// the timer keeps timing.
TEST_F(TimerTest, ARecordThatThrowsDropsItsSpan) {
  Result<GpuTimer> made = GpuTimer::create(device());
  ASSERT_TRUE(made.ok());
  GpuTimer timer = *std::move(made);
  if (!timer.available()) GTEST_SKIP() << "the queue family has no timestamps";
  StageMetrics metrics;
  {
    GpuStageScope stage(&metrics, timer, "thrown");
    bool threw = false;
    try {
      static_cast<void>(device().submit_single_time(
          [](VkCommandBuffer) { throw std::runtime_error("record"); }, stage));
    } catch (const std::runtime_error&) {
      threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(timer.count(), 0U);
    EXPECT_TRUE(timer.available());
    ASSERT_TRUE(device().submit_single_time(fill(), stage).ok());
    EXPECT_EQ(timer.count(), 1U);
  }
  EXPECT_TRUE(row(metrics, "thrown")->has_gpu);
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
  const bool available = a.available();
  GpuTimer b = std::move(a);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(a.valid());
  EXPECT_FALSE(a.available());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(b.valid());
  EXPECT_EQ(b.available(), available);
  Result<GpuTimer> other = GpuTimer::create(device());
  ASSERT_TRUE(other.ok());
  b = *std::move(other);  // over a live timer, whose pool goes
  EXPECT_TRUE(b.valid());
  GpuTimer* self = &b;
  b = std::move(*self);
  EXPECT_TRUE(b.valid());
}

// Timed dispatches keep their spans: two in one submit, each resolved into its
// own row; timed uploads too, inline, staged and reserved, and a timed copy.
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
    if (timer.available()) {
      EXPECT_EQ(timer.count(), 6U);
    }
  }
  for (const char* name : {"first", "second", "uploads"}) {
    const StageRow* r = row(metrics, name);
    ASSERT_NE(r, nullptr) << name;
    EXPECT_GT(r->cpu_ms, 0.0) << name;
    if (timer.available()) {
      EXPECT_TRUE(r->has_gpu) << name;
    }
  }
  // The scope order seeded the rows: first, second, uploads.
  EXPECT_STREQ(metrics.rows()[0].name, "first");

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
  if (timer.available()) {
    EXPECT_TRUE(row(one, "one-shot")->has_gpu);
  }
  {
    GpuStageScope inert(nullptr, timer, "never");
    const Push push{kCount, 3};
    ASSERT_TRUE(
        dispatch(device(), add, &push, sizeof(push), 4, max_groups, &inert)
            .ok());
  }
  EXPECT_EQ(timer.count(), 0U);
}

}  // namespace
}  // namespace volumetric_kit::core
