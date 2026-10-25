// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// HostRing, and CommandBatch::input and ::output, on the one path the
// device's memory gives a ring: in place where device-mapped memory is cached
// (unified memory, CPU implementations), copied through a device-only mirror
// elsewhere (a discrete GPU). Every ring asserts that its path follows that
// fact, and that it is the path the run expects, where VKC_EXPECT_RING_PATH
// names one (`in_place` or `copied`): CI's legs set it, so a driver that
// moved a leg to the other path would fail it rather than leave a path
// untested. Barrier-heavy, so under synchronization validation wherever the
// layer is installed; the lease's threading rule needs ThreadSanitizer to fail
// (core's CI runs these under it).

#include "volumetric_kit/core/vulkan/host_ring.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ring_comp.spv.hpp"
#include "ring_slot.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/compute_kernel.hpp"
#include "volumetric_kit/core/vulkan/compute_util.hpp"
#include "volumetric_kit/core/vulkan/descriptor.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"

namespace volumetric_kit::core {
namespace {

static_assert(!std::is_copy_constructible_v<HostRing> &&
                  !std::is_copy_assignable_v<HostRing>,
              "a ring is not copied");
static_assert(!std::is_move_constructible_v<HostRing> &&
                  !std::is_move_assignable_v<HostRing>,
              "nor moved: create returns it by unique_ptr");
static_assert(std::is_copy_constructible_v<HostSlot> &&
                  std::is_copy_assignable_v<HostSlot>,
              "a slot is a copyable handle");

constexpr std::uint32_t kWords = 256;  // four workgroups of ring.comp
constexpr VkDeviceSize kBytes = kWords * sizeof(std::uint32_t);
constexpr std::uint64_t kLongWaitNs = 10'000'000'000ULL;

struct Push {
  std::uint32_t count;
  std::uint32_t delta;
  std::uint32_t skip;
};

std::vector<std::uint32_t> pattern(std::uint32_t base) {
  std::vector<std::uint32_t> v(kWords);
  for (std::uint32_t i = 0; i < kWords; ++i) v[i] = base + (i * 7U);
  return v;
}

std::vector<std::uint32_t> plus(std::vector<std::uint32_t> v,
                                std::uint32_t delta) {
  for (auto& x : v) x += delta;
  return v;
}

std::vector<std::uint32_t> words_of(const HostSlot& slot,
                                    VkDeviceSize offset = 0) {
  std::vector<std::uint32_t> v(kWords);
  std::memcpy(v.data(), slot.data() + offset, kBytes);
  return v;
}

bool is_invalid(const Status& s) {
  return s.domain() == Status::Code::InvalidArgument;
}

// The path the run expects, `in_place` or `copied`; null or empty for any.
const char* expected_path() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): the test thread
  return std::getenv("VKC_EXPECT_RING_PATH");
}

// The ring-output poison off while this lives, as outside a test process;
// the environment's setting comes back after.
class Unpoisoned {
 public:
  Unpoisoned() : was_(detail::ring_output_poison()) {
    detail::set_ring_output_poison(false);
  }
  ~Unpoisoned() { detail::set_ring_output_poison(was_); }
  Unpoisoned(const Unpoisoned&) = delete;
  Unpoisoned& operator=(const Unpoisoned&) = delete;
  Unpoisoned(Unpoisoned&&) = delete;
  Unpoisoned& operator=(Unpoisoned&&) = delete;

 private:
  bool was_;
};

// Claims `bytes` of `ring`, retrying while another thread still holds every
// slot: that thread's drop is the only order between the two.
Result<HostSlot> claim_when_free(HostRing& ring, VkDeviceSize bytes) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    Result<HostSlot> slot = ring.claim(bytes);
    if (slot.ok() || std::chrono::steady_clock::now() > deadline) return slot;
    std::this_thread::yield();
  }
}

class HostRingTest : public test::VulkanDeviceTest {
 protected:
  test::Validation validation() const override {
    return test::Validation::Sync;
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) return;
    max_groups_ = physical().limits().maxComputeWorkGroupCount[0];
    align_ = physical().limits().minStorageBufferOffsetAlignment;
    range_.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range_.size = sizeof(Push);
    KernelSetBuilder builder(device());
    ASSERT_TRUE(builder
                    .add(kernel_, "test_ring", vkc_test_ring_comp_spv,
                         vkc_test_ring_comp_spv_size, 2, &range_)
                    .ok());
    Result<DescriptorPool> pool = builder.build();
    ASSERT_TRUE(pool.ok()) << pool.status().message();
    pool_ = *std::move(pool);
  }

  void TearDown() override {
    kernel_ = ComputeKernel{};
    pool_ = DescriptorPool{};
    VulkanDeviceTest::TearDown();
  }

  // A ring of `slots` slots of `bytes`, on the path the device's memory gives
  // it: in place exactly where device-mapped memory is cached, which is the
  // path the run expects.
  std::unique_ptr<HostRing> make_ring(VkDeviceSize bytes,
                                      std::uint32_t slots = 2) {
    HostRingDesc desc;
    desc.slot_bytes = bytes;
    desc.slots = slots;
    desc.name = "test.ring";
    Result<std::unique_ptr<HostRing>> ring =
        HostRing::create(device(), allocator(), desc);
    EXPECT_TRUE(ring.ok()) << ring.status().message();
    if (!ring.ok()) return nullptr;
    EXPECT_EQ((*ring)->in_place(), physical().device_mapped_cached());
    const char* expected = expected_path();
    if (expected != nullptr && *expected != '\0') {
      EXPECT_STREQ((*ring)->in_place() ? "in_place" : "copied", expected)
          << "VKC_EXPECT_RING_PATH names another path than "
          << physical().properties().deviceName << "'s memory gives";
    }
    return *std::move(ring);
  }

  // Records ring.comp over kWords words: `src` bound at `src_offset`, `dst`
  // at `dst_offset`.
  Status record(CommandBatch& batch, VkBuffer src, VkDeviceSize src_offset,
                VkBuffer dst, VkDeviceSize dst_offset, std::uint32_t delta,
                std::uint32_t skip = 0) {
    kernel_.set.write_storage_buffer(0, src, src_offset, kBytes);
    kernel_.set.write_storage_buffer(1, dst, dst_offset, kBytes);
    const Push push{kWords, delta, skip};
    return batch.dispatch(kernel_, &push, sizeof(push), group_count(kWords, 64),
                          max_groups_);
  }

  // One frame through `in` (its words at `in_offset`) into `out`, waited by
  // `submit` or by `submit_async`'s wait.
  void frame(const HostSlot& in, VkDeviceSize in_offset, const HostSlot& out,
             std::uint32_t delta, bool async) {
    CommandBatch batch(device(), allocator());
    // The output first: its copy still comes after the kernel.
    Result<VkBuffer> dst = batch.output(out, 0, kBytes);
    Result<VkBuffer> src = batch.input(in, in_offset, kBytes);
    ASSERT_TRUE(dst.ok()) << dst.status().message();
    ASSERT_TRUE(src.ok()) << src.status().message();
    ASSERT_TRUE(record(batch, *src, in_offset, *dst, 0, delta).ok());
    if (async) {
      Result<PendingBatch> pending = batch.submit_async({}, {});
      ASSERT_TRUE(pending.ok()) << pending.status().message();
      ASSERT_TRUE(pending->wait(kLongWaitNs).ok());
    } else {
      ASSERT_TRUE(batch.submit().ok());
    }
  }

  std::uint32_t max_groups_ = 0;
  VkDeviceSize align_ = 0;
  VkPushConstantRange range_{};
  ComputeKernel kernel_;
  DescriptorPool pool_;
};

// --- results ----------------------------------------------------------------

// The kernels read an input at an aligned non-zero offset and the host reads
// their results, through a blocking submit and through a pending batch's
// wait. The output is asked for before the kernel is recorded, and still
// holds what the kernel wrote.
TEST_F(HostRingTest, ResultsComeBackThroughEitherSubmit) {
  std::unique_ptr<HostRing> inputs = make_ring(align_ + kBytes);
  std::unique_ptr<HostRing> results = make_ring(kBytes);
  ASSERT_TRUE(inputs != nullptr && results != nullptr);
  for (const bool async : {false, true}) {
    SCOPED_TRACE(async ? "submit_async" : "submit");
    Result<HostSlot> in = inputs->claim(align_ + kBytes);
    Result<HostSlot> out = results->claim(kBytes);
    ASSERT_TRUE(in.ok() && out.ok());
    const std::vector<std::uint32_t> words = pattern(async ? 50 : 5);
    std::memcpy(in->data() + align_, words.data(), kBytes);
    frame(*in, align_, *out, 3, async);
    EXPECT_EQ(words_of(*out), plus(words, 3));
  }
}

// In place a batch of only inputs and outputs records nothing, so it submits
// nothing; copied it submits its copies, behind the barrier before the
// output's and the final one.
TEST_F(HostRingTest, ABatchOfOnlySlotsRecordsWhatItsPathCopies) {
  std::unique_ptr<HostRing> ring = make_ring(kBytes);
  ASSERT_NE(ring, nullptr);
  Result<HostSlot> in = ring->claim(kBytes);
  Result<HostSlot> out = ring->claim(kBytes);
  ASSERT_TRUE(in.ok() && out.ok());
  const DeviceCounters before = device().counters();
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.input(*in, 0, kBytes).ok());
  ASSERT_TRUE(batch.output(*out, 0, kBytes).ok());
  ASSERT_TRUE(batch.submit().ok());
  const DeviceCounters after = device().counters();
  const bool copied = !ring->in_place();
  EXPECT_EQ(after.submits - before.submits, copied ? 1U : 0U);
  EXPECT_EQ(after.barriers - before.barriers, copied ? 2U : 0U);
  EXPECT_EQ(after.dispatches - before.dispatches, 0U);
  EXPECT_EQ(after.staging_buffers - before.staging_buffers, 0U);
}

// A word the kernels skip shows the poison, not the earlier use's value: the
// same results slot, written whole by one frame, then every fourth word
// skipped by the next.
TEST_F(HostRingTest, AWordTheKernelsSkipShowsThePoison) {
  std::unique_ptr<HostRing> inputs = make_ring(kBytes, 1);
  std::unique_ptr<HostRing> results = make_ring(kBytes, 1);
  ASSERT_TRUE(inputs != nullptr && results != nullptr);
  const std::vector<std::uint32_t> words = pattern(9);
  for (const std::uint32_t skip : {0U, 4U}) {
    Result<HostSlot> in = inputs->claim(kBytes);
    Result<HostSlot> out = results->claim(kBytes);
    ASSERT_TRUE(in.ok() && out.ok());
    std::memcpy(in->data(), words.data(), kBytes);
    CommandBatch batch(device(), allocator());
    Result<VkBuffer> src = batch.input(*in, 0, kBytes);
    Result<VkBuffer> dst = batch.output(*out, 0, kBytes);
    ASSERT_TRUE(src.ok() && dst.ok());
    ASSERT_TRUE(record(batch, *src, 0, *dst, 0, 1 + skip, skip).ok());
    ASSERT_TRUE(batch.submit().ok());
    const std::vector<std::uint32_t> got = words_of(*out);
    for (std::uint32_t i = 0; i < kWords; ++i) {
      const bool skipped = skip != 0 && i % skip == 0;
      ASSERT_EQ(got[i],
                skipped ? detail::kRingOutputPoison : words[i] + 1 + skip)
          << "word " << i << ", skip " << skip;
    }
  }
}

// Outside a test process nothing is poisoned or recorded for it: a skipped
// word keeps the earlier use's value, and a batch that opens with its kernel
// records the final barrier in place; copied, one more before the output's
// copy, and none for a fill.
TEST_F(HostRingTest, WithoutThePoisonASkippedWordKeepsItsValue) {
  EXPECT_TRUE(detail::ring_output_poison());  // the environment's
  const Unpoisoned unpoisoned;
  std::unique_ptr<HostRing> results = make_ring(kBytes, 1);
  ASSERT_NE(results, nullptr);
  // The kernel reads a device buffer, so nothing is recorded before it.
  Result<Buffer> src = device_storage_buffer(allocator(), kBytes);
  ASSERT_TRUE(src.ok()) << src.status().message();
  const std::vector<std::uint32_t> words = pattern(11);
  {
    CommandBatch upload(device(), allocator());
    ASSERT_TRUE(upload.upload(*src, 0, words.data(), kBytes).ok());
    ASSERT_TRUE(upload.submit().ok());
  }
  for (const std::uint32_t skip : {0U, 4U}) {
    Result<HostSlot> out = results->claim(kBytes);
    ASSERT_TRUE(out.ok());
    const DeviceCounters before = device().counters();
    CommandBatch batch(device(), allocator());
    Result<VkBuffer> dst = batch.output(*out, 0, kBytes);
    ASSERT_TRUE(dst.ok());
    ASSERT_TRUE(record(batch, src->handle(), 0, *dst, 0, 1 + skip, skip).ok());
    ASSERT_TRUE(batch.submit().ok());
    EXPECT_EQ(device().counters().barriers - before.barriers,
              results->in_place() ? 1U : 2U)
        << "skip " << skip;
    const std::vector<std::uint32_t> got = words_of(*out);
    for (std::uint32_t i = 0; i < kWords; ++i) {
      const bool skipped = skip != 0 && i % skip == 0;
      ASSERT_EQ(got[i], words[i] + (skipped ? 1U : 1U + skip))
          << "word " << i << ", skip " << skip;
    }
  }
}

// --- slots ------------------------------------------------------------------

// Every slot held -- by a copy, or by a batch -- refuses a claim, naming the
// count; the last copy's drop frees one.
TEST_F(HostRingTest, ClaimRefusesWhileEverySlotIsHeld) {
  std::unique_ptr<HostRing> ring = make_ring(kBytes, 2);
  ASSERT_NE(ring, nullptr);
  Result<HostSlot> a = ring->claim(kBytes);
  Result<HostSlot> b = ring->claim(kBytes);
  ASSERT_TRUE(a.ok() && b.ok());
  const Result<HostSlot> none = ring->claim(kBytes);
  ASSERT_TRUE(is_invalid(none.status()));
  EXPECT_NE(none.status().message().find("all 2 slots of test.ring are held"),
            std::string::npos)
      << none.status().message();
  HostSlot copy = *b;
  *a = HostSlot{};
  *b = HostSlot{};
  EXPECT_TRUE(ring->claim(kBytes).ok());  // a's, dropped again at once
  const Result<HostSlot> again = ring->claim(kBytes);
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(ring->claim(kBytes).ok());  // b's: its copy holds it
  copy = HostSlot{};
  EXPECT_TRUE(ring->claim(kBytes).ok());
}

// A batch holds the slots it binds: until it is destroyed unsubmitted, and a
// pending batch until its wait.
TEST_F(HostRingTest, ABatchHoldsItsSlotsUntilItResolves) {
  std::unique_ptr<HostRing> ring = make_ring(kBytes, 1);
  ASSERT_NE(ring, nullptr);
  {
    Result<HostSlot> slot = ring->claim(kBytes);
    ASSERT_TRUE(slot.ok());
    CommandBatch batch(device(), allocator());
    ASSERT_TRUE(batch.input(*slot, 0, kBytes).ok());
    *slot = HostSlot{};
    EXPECT_FALSE(ring->claim(kBytes).ok());
  }
  EXPECT_TRUE(ring->claim(kBytes).ok());

  Result<Buffer> dst = device_storage_buffer(allocator(), kBytes);
  ASSERT_TRUE(dst.ok());
  Result<HostSlot> slot = ring->claim(kBytes);
  ASSERT_TRUE(slot.ok());
  std::memset(slot->data(), 0, kBytes);
  CommandBatch batch(device(), allocator());
  Result<VkBuffer> src = batch.input(*slot, 0, kBytes);
  ASSERT_TRUE(src.ok());
  ASSERT_TRUE(record(batch, *src, 0, dst->handle(), 0, 1).ok());
  *slot = HostSlot{};
  Result<PendingBatch> pending = batch.submit_async({}, {});
  ASSERT_TRUE(pending.ok()) << pending.status().message();
  EXPECT_FALSE(ring->claim(kBytes).ok());
  ASSERT_TRUE(pending->wait(kLongWaitNs).ok());
  EXPECT_TRUE(ring->claim(kBytes).ok());
}

// The smallest free slot that fits comes back as it was; with none, the
// smallest free slot is replaced, by one counted placement of 1.5x, and a held
// slot's bytes are never touched.
TEST_F(HostRingTest, ClaimTakesTheSmallestFitElseRegrowsTheSmallestFreeSlot) {
  std::unique_ptr<HostRing> ring = make_ring(kBytes, 3);
  ASSERT_NE(ring, nullptr);
  const DeviceCounters before = device().counters();
  Result<HostSlot> big = ring->claim(2 * kBytes);
  ASSERT_TRUE(big.ok());
  EXPECT_EQ(big->capacity(), 3 * kBytes);
  EXPECT_EQ(device().counters().staging_buffers - before.staging_buffers, 1U);
  std::memset(big->data(), 0xB1, 3 * kBytes);
  *big = HostSlot{};

  Result<HostSlot> small = ring->claim(kBytes);
  ASSERT_TRUE(small.ok());
  EXPECT_EQ(small->capacity(), kBytes);  // not the big one
  std::memset(small->data(), 0x5A, kBytes);
  Result<HostSlot> again = ring->claim(2 * kBytes);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again->capacity(), 3 * kBytes);  // the big one, kept
  EXPECT_EQ(again->data()[(3 * kBytes) - 1], 0xB1);
  EXPECT_EQ(device().counters().staging_buffers - before.staging_buffers, 1U);

  // Only the third slot is free: it grows, and the held two stay as written.
  Result<HostSlot> bigger = ring->claim(4 * kBytes);
  ASSERT_TRUE(bigger.ok());
  EXPECT_EQ(bigger->capacity(), 6 * kBytes);
  EXPECT_EQ(device().counters().staging_buffers - before.staging_buffers, 2U);
  EXPECT_EQ(small->data()[kBytes - 1], 0x5A);
  EXPECT_EQ(again->data()[0], 0xB1);

  // Two free slots, of 1x and 3x, both too small: the smaller is replaced,
  // and the bigger then comes back as it was.
  *small = HostSlot{};
  *again = HostSlot{};
  Result<HostSlot> biggest = ring->claim(8 * kBytes);
  ASSERT_TRUE(biggest.ok());
  EXPECT_EQ(biggest->capacity(), 12 * kBytes);
  EXPECT_EQ(device().counters().staging_buffers - before.staging_buffers, 3U);
  Result<HostSlot> kept = ring->claim(3 * kBytes);
  ASSERT_TRUE(kept.ok());
  EXPECT_EQ(kept->capacity(), 3 * kBytes);
  EXPECT_EQ(kept->data()[0], 0xB1);
  EXPECT_EQ(device().counters().staging_buffers - before.staging_buffers, 3U);
}

// A hundred frames after create allocate nothing: the slots are placed once,
// each counted, and every frame after that is one submit with no staging --
// one dispatch and the final barrier in place; copied, a barrier more before
// the kernel and one before the output's copy.
TEST_F(HostRingTest, AHundredFramesAllocateNothingAfterCreate) {
  const DeviceCounters created = device().counters();
  std::unique_ptr<HostRing> inputs = make_ring(kBytes);
  std::unique_ptr<HostRing> results = make_ring(kBytes);
  ASSERT_TRUE(inputs != nullptr && results != nullptr);
  EXPECT_EQ(device().counters().staging_buffers - created.staging_buffers, 4U);
  const DeviceCounters before = device().counters();
  for (std::uint32_t f = 0; f < 100; ++f) {
    Result<HostSlot> in = inputs->claim(kBytes);
    Result<HostSlot> out = results->claim(kBytes);
    ASSERT_TRUE(in.ok() && out.ok()) << "frame " << f;
    const std::vector<std::uint32_t> words = pattern(f);
    std::memcpy(in->data(), words.data(), kBytes);
    frame(*in, 0, *out, f, false);
    ASSERT_EQ(words_of(*out), plus(words, f)) << "frame " << f;
  }
  const DeviceCounters after = device().counters();
  EXPECT_EQ(after.staging_buffers - before.staging_buffers, 0U);
  EXPECT_EQ(after.submits - before.submits, 100U);
  EXPECT_EQ(after.dispatches - before.dispatches, 100U);
  EXPECT_EQ(after.barriers - before.barriers, inputs->in_place() ? 100U : 300U);
}

// A slot outlives its ring, its buffers freed with its last copy (ASan and
// LSan check it on the sanitizer legs).
TEST_F(HostRingTest, ASlotOutlivesItsRing) {
  HostSlot in;
  HostSlot out;
  {
    std::unique_ptr<HostRing> inputs = make_ring(kBytes, 1);
    std::unique_ptr<HostRing> results = make_ring(kBytes, 1);
    ASSERT_TRUE(inputs != nullptr && results != nullptr);
    Result<HostSlot> a = inputs->claim(kBytes);
    Result<HostSlot> b = results->claim(kBytes);
    ASSERT_TRUE(a.ok() && b.ok());
    in = *a;
    out = *b;
  }
  const std::vector<std::uint32_t> words = pattern(2);
  std::memcpy(in.data(), words.data(), kBytes);
  frame(in, 0, out, 4, false);
  EXPECT_EQ(words_of(out), plus(words, 4));
}

// A moved batch takes its slots with it, and the moved-from one refuses.
TEST_F(HostRingTest, AMovedBatchKeepsItsSlots) {
  std::unique_ptr<HostRing> ring = make_ring(kBytes, 1);
  std::unique_ptr<HostRing> other = make_ring(kBytes, 1);
  ASSERT_TRUE(ring != nullptr && other != nullptr);
  Result<HostSlot> slot = ring->claim(kBytes);
  Result<HostSlot> spare = other->claim(kBytes);
  ASSERT_TRUE(slot.ok() && spare.ok());
  CommandBatch batch(device(), allocator());
  ASSERT_TRUE(batch.input(*slot, 0, kBytes).ok());
  *slot = HostSlot{};
  CommandBatch moved = std::move(batch);
  EXPECT_FALSE(ring->claim(kBytes).ok());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(is_invalid(batch.input(*spare, 0, kBytes).status()));
  EXPECT_TRUE(is_invalid(batch.output(*spare, 0, kBytes).status()));
  moved = CommandBatch(device(), allocator());  // drops what it held
  EXPECT_TRUE(ring->claim(kBytes).ok());
}

TEST_F(HostRingTest, RefusesBadArguments) {
  HostRingDesc desc;
  desc.slot_bytes = 0;
  EXPECT_TRUE(is_invalid(
      HostRing::create(device(), allocator(), desc).status()));  // no bytes
  desc.slot_bytes = kBytes;
  desc.slots = 0;
  EXPECT_TRUE(is_invalid(
      HostRing::create(device(), allocator(), desc).status()));  // no slots
  desc.slots = 2;
  {
    Device moved = std::move(device());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_TRUE(
        is_invalid(HostRing::create(device(), allocator(), desc).status()));
    device() = std::move(moved);
  }
  {
    Allocator moved = std::move(allocator());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_TRUE(
        is_invalid(HostRing::create(device(), allocator(), desc).status()));
    allocator() = std::move(moved);
  }

  std::unique_ptr<HostRing> ring = make_ring(kBytes);
  ASSERT_NE(ring, nullptr);
  EXPECT_TRUE(is_invalid(ring->claim(0).status()));
  const HostSlot empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.data(), nullptr);
  EXPECT_EQ(empty.capacity(), 0U);
  Result<HostSlot> slot = ring->claim(kBytes);
  ASSERT_TRUE(slot.ok());
  const auto refused = [&](auto&& bind) {
    CommandBatch batch(device(), allocator());
    const Status first = bind(batch).status();
    return is_invalid(first) && is_invalid(batch.submit());
  };
  for (const bool out : {false, true}) {
    SCOPED_TRACE(out ? "output" : "input");
    const auto call = [out](CommandBatch& b, const HostSlot& s, VkDeviceSize at,
                            VkDeviceSize bytes) {
      return out ? b.output(s, at, bytes) : b.input(s, at, bytes);
    };
    EXPECT_TRUE(refused([&](CommandBatch& b) { return call(b, empty, 0, 4); }));
    EXPECT_TRUE(refused([&](CommandBatch& b) { return call(b, *slot, 0, 0); }));
    EXPECT_TRUE(
        refused([&](CommandBatch& b) { return call(b, *slot, kBytes, 4); }));
    EXPECT_TRUE(
        refused([&](CommandBatch& b) { return call(b, *slot, 4, kBytes); }));
    EXPECT_TRUE(refused(
        [&](CommandBatch& b) { return call(b, *slot, ~VkDeviceSize{0}, 8); }));
    // A poisoned batch returns its first refusal; a submitted one refuses.
    CommandBatch poisoned(device(), allocator());
    const Status first = call(poisoned, empty, 0, 4).status();
    const Status later = call(poisoned, *slot, 0, kBytes).status();
    EXPECT_TRUE(is_invalid(later));
    EXPECT_EQ(later.message(), first.message());
    CommandBatch submitted(device(), allocator());
    ASSERT_TRUE(submitted.submit().ok());
    EXPECT_TRUE(is_invalid(call(submitted, *slot, 0, kBytes).status()));
  }
  // None of the refusals kept the slot: dropped, it is free again.
  *slot = HostSlot{};
  const Result<HostSlot> a = ring->claim(kBytes);
  const Result<HostSlot> b = ring->claim(kBytes);
  EXPECT_TRUE(a.ok() && b.ok());
}

// --- threads ----------------------------------------------------------------

// A slot dropped on another thread comes back to claim with what that thread
// did to it, ordered by the lease alone: under ThreadSanitizer a missing
// order is a reported race, which fails the test. (a) A one-slot ring whose
// slot a thread writes, binds in a batch and drops; every eighth frame asks
// for more than it holds, so the claim regrows it, writing what the thread's
// batch read. (b) A pending batch that a thread waits, reads the results of
// and drops, while this thread claims the slots for the next frame.
TEST_F(HostRingTest, ASlotDroppedOnAnotherThreadIsClaimedWithItsBytes) {
  {
    SCOPED_TRACE("(a) a slot written and dropped on a thread");
    std::unique_ptr<HostRing> ring = make_ring(kBytes, 1);
    ASSERT_NE(ring, nullptr);
    std::thread holder;
    VkDeviceSize capacity = kBytes;
    for (std::uint32_t f = 0; f < 64; ++f) {
      const bool grow = f % 8 == 7;
      Result<HostSlot> slot =
          claim_when_free(*ring, grow ? capacity + 4 : VkDeviceSize{4});
      if (!slot.ok()) {
        ADD_FAILURE() << "frame " << f << ": " << slot.status().message();
        break;
      }
      if (f > 0 && !grow) {
        std::uint32_t last = 0;
        std::memcpy(&last, slot->data(), sizeof(last));
        EXPECT_EQ(last, f - 1);
      }
      capacity = slot->capacity();
      // Joined only now, so the join orders nothing above.
      if (holder.joinable()) holder.join();
      holder = std::thread([this, held = *std::move(slot), f] {
        std::memcpy(held.data(), &f, sizeof(f));
        CommandBatch batch(device(), allocator());
        EXPECT_TRUE(batch.input(held, 0, VkDeviceSize{sizeof(f)}).ok());
      });
    }
    if (holder.joinable()) holder.join();
  }
  {
    SCOPED_TRACE("(b) a pending batch waited and dropped on a thread");
    std::unique_ptr<HostRing> inputs = make_ring(kBytes, 1);
    std::unique_ptr<HostRing> results = make_ring(kBytes, 1);
    ASSERT_TRUE(inputs != nullptr && results != nullptr);
    std::thread waiter;
    for (std::uint32_t f = 0; f < 16; ++f) {
      Result<HostSlot> in = claim_when_free(*inputs, kBytes);
      Result<HostSlot> out = claim_when_free(*results, kBytes);
      if (!in.ok() || !out.ok()) {
        ADD_FAILURE() << "frame " << f << ": " << in.status().message() << "; "
                      << out.status().message();
        break;
      }
      // The host writes both, after the waiter read the results.
      std::memset(out->data(), 0, kBytes);
      const std::vector<std::uint32_t> words = pattern(f);
      std::memcpy(in->data(), words.data(), kBytes);
      CommandBatch batch(device(), allocator());
      Result<VkBuffer> src = batch.input(*in, 0, kBytes);
      Result<VkBuffer> dst = batch.output(*out, 0, kBytes);
      const bool recorded =
          src.ok() && dst.ok() && record(batch, *src, 0, *dst, 0, f).ok();
      Result<PendingBatch> pending =
          recorded
              ? batch.submit_async({}, {})
              : Result<PendingBatch>(Status::invalid_argument("unrecorded"));
      if (waiter.joinable()) waiter.join();
      if (!pending.ok()) {
        ADD_FAILURE() << "frame " << f << ": " << pending.status().message();
        break;
      }
      waiter = std::thread(
          [ran = *std::move(pending), read = *out, words, f]() mutable {
            EXPECT_TRUE(ran.wait(kLongWaitNs).ok());
            EXPECT_EQ(words_of(read), plus(words, f));
          });
    }
    if (waiter.joinable()) waiter.join();
  }
}

}  // namespace
}  // namespace volumetric_kit::core
