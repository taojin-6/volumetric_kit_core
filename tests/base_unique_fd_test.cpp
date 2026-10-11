// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/unique_fd.hpp"

#include <unistd.h>

#include <utility>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

// Whether `fd` names an open descriptor: only an open one duplicates.
bool is_open(int fd) {
  const int copy = dup(fd);
  if (copy < 0) return false;
  close(copy);
  return true;
}

TEST(UniqueFdTest, ClosesWhatItOwnsUnlessReleased) {
  int ends[2] = {-1, -1};
  ASSERT_EQ(pipe(ends), 0);
  {
    UniqueFd read_end(ends[0]);
    UniqueFd moved = std::move(read_end);
    // The moved-from state is what is checked.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(read_end.valid());
    EXPECT_EQ(moved.get(), ends[0]);
    // An assignment closes what the target held.
    UniqueFd write_end(ends[1]);
    moved = std::move(write_end);
    EXPECT_FALSE(is_open(ends[0]));
    EXPECT_EQ(moved.get(), ends[1]);
  }
  EXPECT_FALSE(is_open(ends[1]));  // closed with its owner

  ASSERT_EQ(pipe(ends), 0);
  UniqueFd kept(ends[0]);
  kept.reset();
  EXPECT_FALSE(is_open(ends[0]));
  EXPECT_FALSE(kept.valid());
  // Released, as to an import that took it: left open, and the caller's.
  UniqueFd handed(ends[1]);
  EXPECT_EQ(handed.release(), ends[1]);
  EXPECT_FALSE(handed.valid());
  EXPECT_TRUE(is_open(ends[1]));
  EXPECT_EQ(close(ends[1]), 0);
}

}  // namespace
}  // namespace volumetric_kit::core
