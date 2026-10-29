// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_allocation_failure (test_allocation_failure.hpp) in a test that is
// a main() of its own: measuring records the last allocation's size and site
// and the largest one; an armed failure throws std::bad_alloc from the next
// allocation of its size -- at its site, when one is given -- once, and
// nothrow new returns null for it; and the state is the calling thread's
// alone. The sizes are odd, so no allocation of the runtime's takes them.

#include <cstddef>
#include <cstdio>
#include <new>
#include <thread>

#include "volumetric_kit/core/testing/test_allocation_failure.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace test = volumetric_kit::core::test;

namespace {

constexpr std::size_t kSize = 4093;
constexpr std::size_t kOther = 4099;
constexpr std::size_t kLarger = 8191;

// Where each allocation lands, so the compiler keeps it, and how many each
// site made, which keeps the two functions apart: a compiler that folds
// identical functions would give both one return address.
void* volatile sink = nullptr;
volatile int here_calls = 0;
volatile int there_calls = 0;

// Two call sites of their own, so the replacement sees a return address per
// function. Each returns whether its allocation succeeded.
[[gnu::noinline]] bool allocate_here(std::size_t bytes) {
  here_calls = here_calls + 1;
  try {
    sink = ::operator new(bytes);
  } catch (const std::bad_alloc&) {
    return false;
  }
  ::operator delete(sink);
  return true;
}

[[gnu::noinline]] bool allocate_there(std::size_t bytes) {
  there_calls = there_calls + 2;
  try {
    sink = ::operator new(bytes);
  } catch (const std::bad_alloc&) {
    return false;
  }
  ::operator delete(sink);
  return true;
}

int measures() {
  auto& failure = test::allocation_failure;
  failure = {};
  failure.measure = true;
  const bool allocated =
      allocate_here(kSize) && allocate_there(kLarger) && allocate_here(kOther);
  failure.measure = false;
  CHECK(allocated);
  CHECK(failure.bytes == kOther);
  CHECK(failure.measured_site != nullptr);
  CHECK(failure.largest == kLarger);
  CHECK(failure.largest_site != nullptr);
  CHECK(failure.largest_site != failure.measured_site);
  CHECK(!failure.injected);
  failure = {};
  return 0;
}

int fails_by_size() {
  auto& failure = test::allocation_failure;
  failure = {};
  failure.bytes = kSize;
  failure.armed = true;
  CHECK(allocate_here(kOther));  // another size passes
  CHECK(!failure.injected);
  CHECK(!allocate_here(kSize));
  CHECK(failure.injected);
  CHECK(!failure.armed);
  CHECK(failure.injected_site != nullptr);
  CHECK(allocate_here(kSize));  // once
  failure = {};
  return 0;
}

int fails_at_site() {
  auto& failure = test::allocation_failure;
  failure = {};
  failure.measure = true;
  CHECK(allocate_there(kSize));
  failure.measure = false;
  const void* there = failure.measured_site;
  CHECK(there != nullptr);
  failure.site = there;
  failure.armed = true;
  CHECK(allocate_here(kSize));  // the same size elsewhere passes
  CHECK(!failure.injected);
  CHECK(!allocate_there(kSize));
  CHECK(failure.injected);
  CHECK(failure.injected_site == there);
  failure = {};
  return 0;
}

int nothrow_returns_null() {
  auto& failure = test::allocation_failure;
  failure = {};
  failure.bytes = kSize;
  failure.armed = true;
  void* p = ::operator new(kSize, std::nothrow);
  const bool injected = failure.injected;
  failure = {};
  ::operator delete(p);
  CHECK(p == nullptr);
  CHECK(injected);
  return 0;
}

int stays_on_its_thread() {
  auto& failure = test::allocation_failure;
  failure = {};
  failure.bytes = kSize;
  failure.armed = true;
  bool other_armed = true;
  bool other_allocated = false;
  bool other_injected = true;
  std::thread worker([&] {
    other_armed = test::allocation_failure.armed;
    other_allocated = allocate_here(kSize);
    other_injected = test::allocation_failure.injected;
  });
  worker.join();
  const bool still_armed = failure.armed;
  const bool injected = failure.injected;
  failure = {};
  CHECK(!other_armed);
  CHECK(other_allocated);
  CHECK(!other_injected);
  CHECK(still_armed);
  CHECK(!injected);
  return 0;
}

}  // namespace

int main() {
  if (measures() != 0) return 1;
  if (fails_by_size() != 0) return 1;
  if (fails_at_site() != 0) return 1;
  if (nothrow_returns_null() != 0) return 1;
  if (stays_on_its_thread() != 0) return 1;
  std::puts("test_allocation_failure: OK");
  return 0;
}
