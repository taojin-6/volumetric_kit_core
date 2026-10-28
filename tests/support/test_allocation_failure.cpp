// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/testing/test_allocation_failure.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace volumetric_kit::core::test {
thread_local AllocationFailureState allocation_failure;
}  // namespace volumetric_kit::core::test

// The standard library declares these replaceable functions with reserved
// parameter names; application definitions use ordinary portable names.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t bytes) {
  const void* site = __builtin_return_address(0);
  auto& failure = volumetric_kit::core::test::allocation_failure;
  if (failure.measure) {
    failure.bytes = bytes;
    failure.measured_site = site;
    if (bytes > failure.largest) {
      failure.largest = bytes;
      failure.largest_site = site;
    }
  }
  if (failure.armed && bytes == failure.bytes &&
      (failure.site == nullptr || failure.site == site)) {
    failure.armed = false;  // The error Status can allocate its message.
    failure.injected = true;
    failure.injected_site = site;
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(bytes == 0 ? 1 : bytes)) return p;
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
// A linked library may use sized delete even when this compiler does not
// emit it, so both forms must free through the replacement above.
void operator delete(void* p, std::size_t /*size*/) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p, std::size_t /*size*/) noexcept {
  ::operator delete[](p);
}

// The default nothrow operators can use the sanitizer's allocator directly.
// Route them through the same allocation/deallocation pair as throwing new.
void* operator new(std::size_t bytes, const std::nothrow_t& /*tag*/) noexcept {
  try {
    return ::operator new(bytes);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes,
                     const std::nothrow_t& /*tag*/) noexcept {
  return ::operator new(bytes, std::nothrow);
}
void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p, const std::nothrow_t& /*tag*/) noexcept {
  ::operator delete[](p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)
