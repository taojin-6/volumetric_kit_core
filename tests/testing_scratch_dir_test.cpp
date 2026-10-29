// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_base's scratch directory (scratch_dir.hpp): each Scratch is a new
// directory under the system's temporary one, named by its prefix and six more
// characters; read_bytes returns a file's bytes, or none for a file that
// cannot be read; and the directory goes, with the files and directories in
// it, when the Scratch does.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/testing/scratch_dir.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPrefix = "vkc_scratch_dir_test_";

int run() {
  fs::path gone;
  {
    const test::Scratch scratch{std::string(kPrefix)};
    const test::Scratch other{std::string(kPrefix)};
    CHECK(fs::is_directory(scratch.path));
    CHECK(fs::is_empty(scratch.path));
    CHECK(
        fs::equivalent(scratch.path.parent_path(), fs::temp_directory_path()));
    const std::string name = scratch.path.filename().string();
    CHECK(name.size() == kPrefix.size() + 6);
    CHECK(name.compare(0, kPrefix.size(), kPrefix) == 0);
    CHECK(fs::is_directory(other.path));
    CHECK(!fs::equivalent(scratch.path, other.path));

    const std::string bytes("\x00\x01\xFFscratch", 10);
    const std::string file = (scratch.path / "take.bin").string();
    CHECK(vkc::write_file(file, bytes, vkc::WriteMode::CreateNew).ok());
    CHECK(test::read_bytes(file) ==
          std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    CHECK(test::read_bytes((scratch.path / "missing.bin").string()).empty());

    // Removed with everything in it, a directory included.
    const fs::path nested = scratch.path / "nested";
    CHECK(vkc::create_new_directory(nested.string()).ok());
    CHECK(vkc::write_file((nested / "inner.bin").string(), bytes,
                          vkc::WriteMode::CreateNew)
              .ok());
    gone = scratch.path;
  }
  CHECK(!gone.empty());
  CHECK(!fs::exists(gone));
  return 0;
}

}  // namespace

// An exception that escapes aborts the test, which fails it.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  if (run() != 0) return 1;
  std::puts("scratch_dir: OK");
  return 0;
}
