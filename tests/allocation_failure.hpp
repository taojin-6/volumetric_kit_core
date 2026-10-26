// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

#include <cstddef>

namespace volumetric_kit::core::test {

struct AllocationSite {
  std::size_t size = 0;
  const void* caller = nullptr;
};

// Observe this executable's ordinary new without changing its behavior;
// shared-library driver/layer allocations are excluded. With a static core,
// the final allocation in resource creation is its returned owner's deleter.
void record_allocations() noexcept;
AllocationSite finish_allocations() noexcept;

// Fail this thread's allocations after `allowed` successful ones. Arm only
// around host-only API work, never a driver call or a GoogleTest assertion.
class AllocationFailure {
 public:
  explicit AllocationFailure(std::size_t allowed = 0,
                             bool persistent = false) noexcept;
  // Fail only this exact ordinary-new size and call site, leaving driver
  // and VMA allocation sites unaffected.
  explicit AllocationFailure(AllocationSite site) noexcept;
  ~AllocationFailure();
  AllocationFailure(const AllocationFailure&) = delete;
  AllocationFailure& operator=(const AllocationFailure&) = delete;
  AllocationFailure(AllocationFailure&&) = delete;
  AllocationFailure& operator=(AllocationFailure&&) = delete;

  // Disarm before assertions, and report whether an allocation failed.
  bool stop() noexcept;
  // Allocation attempts observed before stop(), including a failed one.
  std::size_t allocations() const noexcept { return allocations_; }

 private:
  bool stopped_ = false;
  bool triggered_ = false;
  std::size_t allocations_ = 0;
};

}  // namespace volumetric_kit::core::test
