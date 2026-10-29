// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file test_allocation_failure.hpp
/// @brief Measure a thread's allocations and fail one of them, through a
///        replacement `operator new`, with no test framework and no Vulkan
///        (`volumetric_kit::core_test_allocation_failure`).
///
/// Link `volumetric_kit::core_test_allocation_failure` only into tests that
/// inject failures or measure allocations: it replaces the global allocation
/// and deallocation functions of the whole binary. The replacements are
/// compiled apart from their callers so that GCC does not inline delete's
/// free and mistake it for a mismatched deallocation.
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// auto& failure = test::allocation_failure;
/// failure.measure = true;
/// run_once();  // the call under test; its last allocation is recorded
/// failure.measure = false;
/// failure.site = failure.measured_site;  // that allocation, and no other
/// failure.armed = true;
/// const Status status = run_once();      // OutOfMemory, if it holds
/// CHECK(failure.injected);
/// failure = {};
/// @endcode
///
/// TODO: one allocation-failure helper for the family. The core's own tests
/// keep theirs (tests/allocation_failure.hpp), and gfx, player and calib
/// theirs, until they move to this one or it gains what theirs do.

#include <cstddef>

namespace volumetric_kit::core::test {

/// @brief What this thread's allocations do under the replacement
///        `operator new`: what it measures, and the failure it injects.
struct AllocationFailureState {
  /// Record each allocation's size and site while set.
  bool measure = false;
  /// The last allocation's size while measuring; the size to fail while
  /// armed.
  std::size_t bytes = 0;
  /// The last allocation's call site while measuring.
  const void* measured_site = nullptr;
  /// The largest allocation while measuring.
  std::size_t largest = 0;
  /// Its call site.
  const void* largest_site = nullptr;
  /// Fail the next allocation of @ref bytes (at @ref site, if set) with
  /// `std::bad_alloc`; cleared as it fails, so the error `Status` can
  /// allocate its message.
  bool armed = false;
  /// Whether an allocation was failed.
  bool injected = false;
  /// Optionally, the call site to fail at, captured by measuring, so that an
  /// unrelated allocation of the same size cannot consume the failure.
  const void* site = nullptr;
  /// The call site that was failed.
  const void* injected_site = nullptr;
};

/// @brief This thread's state: injection is limited to the calling thread,
///        so a driver's worker cannot consume it.
extern thread_local AllocationFailureState allocation_failure;

}  // namespace volumetric_kit::core::test
