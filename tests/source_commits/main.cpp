// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Prints what vkc_source_commits wrote, one source a line, as
// source_commits_rebuild.cmake expects it: "<name> [<sha>] <dirty>", then
// " [<override>]" for a package.

#include <cstdio>

#include "source_commits.h"

int main() {
  std::printf("project [%s] %d\n", VKC_SC_PROJECT_SHA, VKC_SC_PROJECT_DIRTY);
  std::printf("dep [%s] %d [%s]\n", VKC_SC_DEP_SHA, VKC_SC_DEP_DIRTY,
              VKC_SC_DEP_OVERRIDE);
  std::printf("local [%s] %d [%s]\n", VKC_SC_LOCAL_SHA, VKC_SC_LOCAL_DIRTY,
              VKC_SC_LOCAL_OVERRIDE);
  return 0;
}
