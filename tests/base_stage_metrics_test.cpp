// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// StageMetrics and StageScope, ported from recon's core_stage_metrics_test:
// pure host, so they run everywhere.

#include "volumetric_kit/core/base/stage_metrics.hpp"

#include <string>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

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

TEST(StageMetrics, TotalsSkipHostBreakdownsButKeepDeviceOnes) {
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

  // A breakdown's device span is a separate dispatch no stage span contains.
  m.add_gpu("extract", 6.0);
  m.add_gpu("  ..meshing", 6.0);
  EXPECT_EQ(m.total_gpu_ms(), 12.0);
  EXPECT_EQ(m.total_gpu_ms("extract"), 6.0);
  EXPECT_EQ(m.total_gpu_ms("  ..meshing"), 6.0);
  EXPECT_EQ(m.total_gpu_ms("integrate"), 12.0);  // unmeasured adds nothing
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

TEST(StageMetrics, MergeCarriesBothHalves) {
  StageMetrics dst;
  dst.add_cpu("extract", 1.0);
  StageMetrics src;
  src.add_cpu("extract", 2.0);
  src.add_gpu("extract", 0.5);
  src.add_cpu("texture", 3.0);  // host-only: gains no has_gpu
  dst.merge(src);
  ASSERT_EQ(dst.rows().size(), 2U);
  EXPECT_EQ(dst.rows()[0].cpu_ms, 3.0);
  EXPECT_EQ(dst.rows()[0].gpu_ms, 0.5);
  EXPECT_TRUE(dst.rows()[0].has_gpu);
  EXPECT_EQ(dst.rows()[1].cpu_ms, 3.0);
  EXPECT_FALSE(dst.rows()[1].has_gpu);
  dst.merge(dst);  // into itself: nothing
  EXPECT_EQ(dst.rows().size(), 2U);
  EXPECT_EQ(dst.rows()[0].cpu_ms, 3.0);
}

}  // namespace
}  // namespace volumetric_kit::core
