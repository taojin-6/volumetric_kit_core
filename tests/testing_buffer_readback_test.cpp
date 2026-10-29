// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_policy's buffer readback (buffer_readback.hpp), on run_on_gpu's
// device: an uploaded buffer reads back as written, whole and from an element
// on; write_back overwrites the elements it is given from its first and
// leaves the rest; and a read past the buffer is refused. Skips without a
// device, unless VKC_REQUIRE_VULKAN_DEVICE is set.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/buffer_readback.hpp"
#include "volumetric_kit/core/testing/gpu_test.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;

namespace {

int round_trips(test::GpuContext& gpu) {
  const std::vector<std::uint32_t> words = {0xA0, 0xA1, 0xA2, 0xA3,
                                            0xA4, 0xA5, 0xA6, 0xA7};
  vkc::Result<vkc::Buffer> buffer = test::upload_device_buffer(
      gpu.device, gpu.allocator, words.data(), words.size() * sizeof(words[0]));
  CHECK(buffer.ok());

  const vkc::Result<std::vector<std::uint32_t>> whole =
      test::read_back<std::uint32_t>(gpu.device, gpu.allocator, *buffer,
                                     words.size());
  CHECK(whole.ok());
  CHECK(*whole == words);
  const vkc::Result<std::vector<std::uint32_t>> tail =
      test::read_back<std::uint32_t>(gpu.device, gpu.allocator, *buffer, 3, 5);
  CHECK(tail.ok());
  CHECK(*tail == std::vector<std::uint32_t>({0xA5, 0xA6, 0xA7}));

  CHECK(test::write_back(gpu.device, gpu.allocator, *buffer,
                         std::vector<std::uint32_t>{0xB2, 0xB3}, 2)
            .ok());
  const vkc::Result<std::vector<std::uint32_t>> written =
      test::read_back<std::uint32_t>(gpu.device, gpu.allocator, *buffer,
                                     words.size());
  CHECK(written.ok());
  CHECK(*written == std::vector<std::uint32_t>(
                        {0xA0, 0xA1, 0xB2, 0xB3, 0xA4, 0xA5, 0xA6, 0xA7}));

  // Elements of another size address the same bytes.
  const vkc::Result<std::vector<std::uint64_t>> pairs =
      test::read_back<std::uint64_t>(gpu.device, gpu.allocator, *buffer, 1, 1);
  CHECK(pairs.ok());
  std::uint32_t halves[2] = {};
  static_assert(sizeof(halves) == sizeof(std::uint64_t));
  std::memcpy(halves, pairs->data(), sizeof(halves));
  CHECK(halves[0] == 0xB2);
  CHECK(halves[1] == 0xB3);

  CHECK(!test::read_back<std::uint32_t>(gpu.device, gpu.allocator, *buffer, 2,
                                        words.size() - 1)
             .ok());
  return 0;
}

}  // namespace

int main() {
  const int code = test::run_on_gpu({}, round_trips);
  if (code == 0) std::puts("buffer_readback: OK");
  return code;
}
