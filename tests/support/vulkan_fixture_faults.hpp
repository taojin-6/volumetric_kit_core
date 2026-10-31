// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Faults the core's own tests inject into the fixture's shared devices
// (tests/testing_vulkan_fixture_probe.cpp). Private to core_test_support: no
// consumer includes it.

namespace volumetric_kit::core::test::detail {

struct FixtureFaults {
  // How many of the next shared devices the fixture makes fail, as a driver's
  // refusal would.
  int failed_creations = 0;
  // Whether the next fixture TearDown finds its device lost.
  bool lost = false;
};

// The process's faults, set and read on the test thread.
FixtureFaults& fixture_faults();

}  // namespace volumetric_kit::core::test::detail
