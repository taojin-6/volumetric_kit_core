// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A sibling's test that is a main() of its own on the core's Vulkan-free test
// support, with no googletest: CHECK, a scratch directory written and read
// back, and an allocation failed on purpose.

#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/testing/scratch_dir.hpp"
#include "volumetric_kit/core/testing/test_allocation_failure.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;

namespace {

int run() {
  const test::Scratch scratch("vkc_consumer_");
  const std::string path = (scratch.path / "bytes.bin").string();
  CHECK(vkc::write_file(path, "core", vkc::WriteMode::CreateNew).ok());
  CHECK(test::read_bytes(path) ==
        std::vector<std::uint8_t>({'c', 'o', 'r', 'e'}));

  auto& failure = test::allocation_failure;
  failure = {};
  failure.bytes = 4093;
  failure.armed = true;
  void* p = ::operator new(4093, std::nothrow);
  const bool injected = failure.injected;
  failure = {};
  ::operator delete(p);
  CHECK(p == nullptr);
  CHECK(injected);
  return 0;
}

}  // namespace

int main() {
  const int code = run();
  if (code == 0) std::puts("check: OK");
  return code;
}
