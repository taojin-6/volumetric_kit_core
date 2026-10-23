// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// StageMetrics, StageScope and WaitScope, and the thread's stack of open
// scopes they join: pure host, so they run everywhere.

#include "volumetric_kit/core/base/stage_metrics.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

// The row named @p name; a missing one fails the test and reads as zero.
StageRow row(const StageMetrics& metrics, const char* name) {
  for (const StageRow& r : metrics.rows()) {
    if (std::string(r.name) == name) return r;
  }
  ADD_FAILURE() << "no row " << name;
  return StageRow{name, 0.0, 0.0, false, 0.0, DeviceTime::None};
}

TEST(StageMetrics, AccumulatesByContentNotPointer) {
  StageMetrics m;
  m.add_cpu("allocate", 1.5);
  m.add_cpu("allocate", 2.5);
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(m.rows()[0].cpu_ms, 4.0);
  // Equal literals need not share an address, across translation units
  // routinely do not; built at run time so the compiler cannot merge them.
  char built[] = "allocate";
  m.add_cpu(built, 1.0);
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(m.rows()[0].cpu_ms, 5.0);
}

TEST(StageMetrics, OnlyAddGpuMarksARowMeasured) {
  StageMetrics m;
  m.add_cpu("integrate", 3.0);
  EXPECT_FALSE(m.rows()[0].has_gpu);
  EXPECT_EQ(m.rows()[0].gpu_ms, 0.0);
  m.add_gpu("integrate", 1.25);
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_TRUE(m.rows()[0].has_gpu);
  EXPECT_EQ(m.rows()[0].cpu_ms, 3.0);
  EXPECT_EQ(m.rows()[0].gpu_ms, 1.25);

  // The other order: a device span may land before the host one.
  StageMetrics n;
  n.add_gpu("meshing", 2.0);
  n.add_cpu("meshing", 8.0);
  ASSERT_EQ(n.rows().size(), 1U);
  EXPECT_EQ(n.rows()[0].gpu_ms, 2.0);
  EXPECT_EQ(n.rows()[0].cpu_ms, 8.0);
  EXPECT_TRUE(n.rows()[0].has_gpu);
}

TEST(StageMetrics, SeedKeepsATablesShape) {
  StageMetrics m;
  m.seed("texture");
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(m.rows()[0].cpu_ms, 0.0);
  EXPECT_FALSE(m.rows()[0].has_gpu);  // seeding measures nothing
  m.add_cpu("texture", 2.0);
  EXPECT_EQ(m.rows().size(), 1U);
}

TEST(StageMetrics, TotalsSkipBreakdowns) {
  StageMetrics m;
  m.add_cpu("extract", 10.0);
  m.add_cpu("  ..meshing", 8.0);
  m.add_cpu("  ..readback", 2.0);
  m.add_cpu("integrate", 10.0);
  ASSERT_EQ(m.rows().size(), 4U);
  // A breakdown restates its stage's host time: counting it would say 30.
  EXPECT_EQ(m.total_cpu_ms(), 20.0);
  EXPECT_EQ(m.total_cpu_ms("integrate"), 10.0);
  EXPECT_EQ(m.total_cpu_ms("nosuchstage"), 20.0);

  // So does its wait...
  m.add_wait("extract", 3.0);
  m.add_wait("  ..readback", 3.0);
  EXPECT_EQ(m.total_wait_ms(), 3.0);
  EXPECT_EQ(m.total_wait_ms("extract"), 0.0);

  // ...and, as tiles nest, its device time: the stage's tiles contain it.
  m.add_gpu("extract", 6.0);
  m.add_gpu("  ..meshing", 6.0);
  EXPECT_EQ(m.total_gpu_ms(), 6.0);
  EXPECT_EQ(m.total_gpu_ms("extract"), 0.0);
  EXPECT_EQ(m.total_gpu_ms("integrate"), 6.0);  // unmeasured adds nothing
}

// A row reads Measured only when all its device work was: a partial sum must
// not pass for the whole stage.
TEST(StageMetrics, DeviceStatesRankAndOnlyMeasuredCounts) {
  StageMetrics m;
  m.add_device("decode", DeviceTime::None, 5.0);
  EXPECT_TRUE(m.empty());  // no device half: no row either
  m.add_gpu("decode", 2.0);
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(m.rows()[0].device, DeviceTime::Measured);
  EXPECT_TRUE(m.rows()[0].has_gpu);
  EXPECT_EQ(m.total_gpu_ms(), 2.0);
  m.add_device("decode", DeviceTime::NotTimed, 9.0);  // its 9 is not counted
  EXPECT_EQ(m.rows()[0].device, DeviceTime::NotTimed);
  EXPECT_FALSE(m.rows()[0].has_gpu);
  EXPECT_EQ(m.rows()[0].gpu_ms, 2.0);
  EXPECT_EQ(m.total_gpu_ms(), 0.0);
  m.add_gpu("decode", 1.0);  // a later measurement does not undo it
  EXPECT_EQ(m.rows()[0].device, DeviceTime::NotTimed);
  m.add_device("decode", DeviceTime::Unavailable);
  EXPECT_EQ(m.rows()[0].device, DeviceTime::Unavailable);
  m.add_device("decode", DeviceTime::NotTimed);
  EXPECT_EQ(m.rows()[0].device, DeviceTime::Unavailable);
}

// The fields added since the first four come last, so a row built
// positionally keeps its meaning, and has_gpu alone still says Measured.
TEST(StageMetrics, APositionalRowKeepsItsMeaning) {
  const StageRow legacy{"pass", 4.0, 1.5, true};
  EXPECT_EQ(legacy.wait_ms, 0.0);
  EXPECT_EQ(legacy.device, DeviceTime::None);
  StageMetrics m;
  m.merge_device({legacy});
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(m.rows()[0].device, DeviceTime::Measured);
  EXPECT_EQ(m.rows()[0].gpu_ms, 1.5);
  EXPECT_EQ(m.rows()[0].cpu_ms, 0.0);  // merge_device leaves host halves
}

TEST(StageMetrics, MergeDeviceTakesOnlyDeviceHalves) {
  StageMetrics m;
  m.add_cpu("mesh", 3.0);
  const StageRow measured{"mesh", 99.0, 2.0, true, 99.0, DeviceTime::Measured};
  const StageRow untimed{"  ..mc", 0.0, 0.0, false, 0.0, DeviceTime::NotTimed};
  const StageRow host_only{"read", 7.0, 0.0, false, 0.0, DeviceTime::None};
  m.merge_device({measured, untimed, host_only});
  ASSERT_EQ(m.rows().size(), 2U);  // a host-only row adds nothing
  EXPECT_EQ(m.rows()[0].cpu_ms, 3.0);
  EXPECT_EQ(m.rows()[0].wait_ms, 0.0);
  EXPECT_EQ(m.rows()[0].gpu_ms, 2.0);
  EXPECT_EQ(m.rows()[1].device, DeviceTime::NotTimed);
}

// --- the thread's stack of open scopes ---------------------------------------

// A wait reaches every scope open around it, a breakdown's included, as its
// host span does; the totals count it once.
TEST(ScopeStack, BlockedTimeReachesEveryOpenScope) {
  StageMetrics m;
  {
    const StageScope stage(m, "decode");
    {
      const StageScope sub(m, "  ..rans");
      note_blocked(2'000'000);  // 2 ms
    }
    note_blocked(1'000'000);
  }
  ASSERT_EQ(m.rows().size(), 2U);
  EXPECT_DOUBLE_EQ(row(m, "decode").wait_ms, 3.0);    // both
  EXPECT_DOUBLE_EQ(row(m, "  ..rans").wait_ms, 2.0);  // its own
  EXPECT_DOUBLE_EQ(m.total_wait_ms(), 3.0);
  // A scope on another set is open around it too.
  StageMetrics helper;
  {
    const StageScope frame(m, "frame");
    const StageScope local(helper, "local");
    note_blocked(1'000'000);
  }
  EXPECT_DOUBLE_EQ(row(helper, "local").wait_ms, 1.0);
  EXPECT_DOUBLE_EQ(row(m, "frame").wait_ms, 1.0);
}

// Nothing open, nothing recorded; a null set never joins the stack, so a
// wait inside an inert scope reaches only the scopes with metrics.
TEST(ScopeStack, NullMetricsRecordNothing) {
  EXPECT_EQ(detail::innermost_scope(), nullptr);
  note_blocked(1'000'000);  // nothing open: a no-op
  StageMetrics m;
  {
    const StageScope inert(nullptr, "inert");
    const WaitScope inert_wait(nullptr, "inert wait");
    EXPECT_EQ(inert.serial(), 0U);
    EXPECT_EQ(detail::innermost_scope(), nullptr);
    note_blocked(1'000'000);
  }
  {
    const StageScope live(m, "live");
    {
      const StageScope inert(nullptr, "inert");
      note_blocked(1'000'000);
    }
  }
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_DOUBLE_EQ(m.rows()[0].wait_ms, 1.0);
  EXPECT_EQ(detail::innermost_scope(), nullptr);
}

// A wait scope is all wait; a core wait inside it is part of its span, so it
// reaches the scopes around it once, when the wait scope closes.
TEST(ScopeStack, AWaitScopeCountsItsSpanOnce) {
  // An hour, reported inside: counted again, it would dwarf any span a
  // loaded machine stretches the 2 ms sleep to.
  constexpr std::int64_t kInsideNs = 3'600'000'000'000;
  StageMetrics m;
  {
    const StageScope frame(m, "frame");
    {
      const WaitScope wait(&m, "wait: GPU behind");
      note_blocked(kInsideNs);  // inside: not counted again
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  ASSERT_EQ(m.rows().size(), 2U);
  const StageRow& frame = row(m, "frame");
  const StageRow& wait = row(m, "wait: GPU behind");
  EXPECT_DOUBLE_EQ(wait.cpu_ms, wait.wait_ms);
  EXPECT_GE(wait.wait_ms, 2.0);
  EXPECT_LT(frame.wait_ms, static_cast<double>(kInsideNs) * 1e-6);
  EXPECT_DOUBLE_EQ(frame.wait_ms, wait.wait_ms);
  EXPECT_GE(frame.cpu_ms, frame.wait_ms);
  EXPECT_EQ(wait.device, DeviceTime::None);
}

// Serials name a scope without a pointer a later one could reuse; a scope
// closed out of order is unlinked where it is.
TEST(ScopeStack, SerialsAreUniqueAndOutOfOrderClosesUnlink) {
  StageMetrics m;
  std::uint64_t first = 0;
  {
    const StageScope a(m, "a");
    first = a.serial();
    EXPECT_NE(first, 0U);
  }
  {
    const StageScope b(m, "b");  // likely the same address as `a`
    EXPECT_NE(b.serial(), first);
  }
  auto outer = std::make_unique<StageScope>(m, "outer");
  auto inner = std::make_unique<StageScope>(m, "inner");
  outer.reset();  // out of order
  note_blocked(1'000'000);
  EXPECT_EQ(detail::innermost_scope()->name, std::string("inner"));
  inner.reset();
  EXPECT_EQ(detail::innermost_scope(), nullptr);
  EXPECT_EQ(row(m, "outer").wait_ms, 0.0);  // it had closed
  EXPECT_DOUBLE_EQ(row(m, "inner").wait_ms, 1.0);
}

// Serials are unique across threads too, so a tag carried to another thread
// matches none of its scopes: two fresh threads' first scopes differ.
TEST(ScopeStack, SerialsAreUniqueAcrossThreads) {
  StageMetrics a;
  StageMetrics b;
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  std::thread([&] {
    const StageScope scope(a, "a");
    first = scope.serial();
  }).join();
  std::thread([&] {
    const StageScope scope(b, "b");
    second = scope.serial();
  }).join();
  EXPECT_NE(first, 0U);
  EXPECT_NE(first, second);
}

// The stack is per thread: a wait on another thread reaches none of this
// one's scopes.
TEST(ScopeStack, OtherThreadsWaitsStayThere) {
  StageMetrics here;
  StageMetrics there;
  {
    const StageScope mine(here, "here");
    std::thread other([&there] {
      const StageScope theirs(there, "there");
      note_blocked(1'000'000);
    });
    other.join();
  }
  EXPECT_EQ(here.rows()[0].wait_ms, 0.0);
  EXPECT_DOUBLE_EQ(there.rows()[0].wait_ms, 1.0);
}

// A device span credited to an open scope lands in its row as it closes.
TEST(ScopeStack, CreditedDeviceTimeLandsAtClose) {
  StageMetrics m;
  {
    const StageScope stage(m, "mesh");
    detail::ScopeFrame* frame = detail::innermost_scope();
    ASSERT_NE(frame, nullptr);
    frame->device = DeviceTime::Measured;
    frame->device_ms = 1.25;
    EXPECT_TRUE(m.rows().empty());  // nothing until it closes
  }
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(m.rows()[0].device, DeviceTime::Measured);
  EXPECT_EQ(m.rows()[0].gpu_ms, 1.25);
}

TEST(StageMetrics, ScopesReportTheirNesting) {
  StageMetrics m;
  EXPECT_FALSE(m.in_stage());
  {
    const StageScope outer(m, "integrate");
    EXPECT_TRUE(m.in_stage());
    {
      const StageScope inner(m, "  ..active set");
      EXPECT_TRUE(m.in_stage());
    }
    EXPECT_TRUE(m.in_stage());  // the inner closing ends nothing outer
  }
  EXPECT_FALSE(m.in_stage());
  {
    const StageScope none(nullptr, "nowhere");
    EXPECT_FALSE(m.in_stage());
  }
}

// The open scopes belong to the object, not its value: a set reset by
// assignment inside a scope stays in it, and a copy starts in none.
TEST(StageMetrics, CopiesAndAssignmentsLeaveTheScopesWhereTheyAre) {
  StageMetrics m;
  {
    const StageScope frame(m, "frame");
    m.add_cpu("stale", 1.0);
    m = StageMetrics{};
    EXPECT_TRUE(m.empty());
    EXPECT_TRUE(m.in_stage());
    const StageMetrics copy = m;
    EXPECT_FALSE(copy.in_stage());
    const StageMetrics moved = std::move(m);
    EXPECT_FALSE(moved.in_stage());
    // The scope stays with the moved-from set it was opened on.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_TRUE(m.in_stage());
  }
  EXPECT_FALSE(m.in_stage());  // closed once, as opened
  {
    const StageScope again(m, "frame");
    EXPECT_TRUE(m.in_stage());
  }
  EXPECT_FALSE(m.in_stage());
}

// Every row is matched by its text, so a null name is a caller's bug.
TEST(StageMetricsDeathTest, ANullNameIsAProgrammerError) {
  EXPECT_DEATH(
      {
        StageMetrics m;
        m.add_cpu(nullptr, 1.0);
      },
      "a row's name is null");
  EXPECT_DEATH(
      {
        StageMetrics m;
        const StageScope scope(m, nullptr);
      },
      "the name is null");
  EXPECT_DEATH(
      {
        StageMetrics m;
        const WaitScope scope(&m, nullptr);
      },
      "the name is null");
}

// A scope opens and closes on one thread: closed on another, it would stay
// on its opener's stack once freed.
TEST(StageMetricsDeathTest, AScopeClosedOnAnotherThreadIsAProgrammerError) {
  EXPECT_DEATH(
      {
        StageMetrics m;
        auto scope = std::make_unique<StageScope>(m, "opened here");
        std::thread([&scope] { scope.reset(); }).join();
      },
      "a thread other than the one it opened on");
}

TEST(StageMetrics, ClearKeepsItUsable) {
  StageMetrics m;
  m.add_cpu("a", 1.0);
  m.clear();
  EXPECT_TRUE(m.empty());
  EXPECT_EQ(m.total_cpu_ms(), 0.0);
  m.add_cpu("b", 2.0);
  EXPECT_EQ(m.rows().size(), 1U);
}

TEST(StageMetrics, AnInertScopeRecordsNothing) {
  StageMetrics m;
  {
    const StageScope inert(static_cast<StageMetrics*>(nullptr), "never");
  }
  EXPECT_TRUE(m.empty());
  {
    const StageScope live(&m, "counted");
  }
  ASSERT_EQ(m.rows().size(), 1U);
  EXPECT_EQ(std::string(m.rows()[0].name), "counted");
  EXPECT_GE(m.rows()[0].cpu_ms, 0.0);
  {
    const StageScope by_reference(m, "counted");
  }
  EXPECT_EQ(m.rows().size(), 1U);
}

TEST(StageMetrics, MergeCarriesEveryHalf) {
  StageMetrics dst;
  dst.add_cpu("extract", 1.0);
  StageMetrics src;
  src.add_cpu("extract", 2.0);
  src.add_wait("extract", 1.5);
  src.add_gpu("extract", 0.5);
  src.add_cpu("texture", 3.0);  // host-only: gains no device half
  src.add_device("  ..mc", DeviceTime::NotTimed);
  dst.merge(src);
  ASSERT_EQ(dst.rows().size(), 3U);
  EXPECT_EQ(dst.rows()[0].cpu_ms, 3.0);
  EXPECT_EQ(dst.rows()[0].wait_ms, 1.5);
  EXPECT_EQ(dst.rows()[0].gpu_ms, 0.5);
  EXPECT_EQ(dst.rows()[0].device, DeviceTime::Measured);
  EXPECT_TRUE(dst.rows()[0].has_gpu);
  EXPECT_EQ(dst.rows()[1].cpu_ms, 3.0);
  EXPECT_EQ(dst.rows()[1].device, DeviceTime::None);
  EXPECT_FALSE(dst.rows()[1].has_gpu);
  EXPECT_EQ(dst.rows()[2].device, DeviceTime::NotTimed);
  dst.merge(dst);  // into itself: nothing
  EXPECT_EQ(dst.rows().size(), 3U);
  EXPECT_EQ(dst.rows()[0].cpu_ms, 3.0);
}

}  // namespace
}  // namespace volumetric_kit::core
