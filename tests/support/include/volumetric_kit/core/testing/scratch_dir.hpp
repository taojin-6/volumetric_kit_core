// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file scratch_dir.hpp
/// @brief A test's own directory for the files it writes, and those files
///        read back, with no test framework and no Vulkan
///        (`volumetric_kit::core_test_base`).
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// const test::Scratch scratch("stream_footer_");
/// const std::string path = (scratch.path / "take.bin").string();
/// // ... write path
/// const std::vector<std::uint8_t> bytes = test::read_bytes(path);
/// @endcode

#include <stdlib.h>  // mkdtemp: POSIX and glibc declare it here,
#include <unistd.h>  // and Apple here.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "volumetric_kit/core/base/file_io.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core::test {

/// @brief A new directory under the system's temporary one, `prefix` and six
///        random characters (`mkdtemp`), removed with everything in it when
///        the scratch goes. A test that cannot have one stops here.
struct Scratch {
  /// @param prefix  The directory name's start.
  explicit Scratch(const std::string& prefix) {
    std::string name =
        (std::filesystem::temp_directory_path() / (prefix + "XXXXXX")).string();
    if (mkdtemp(name.data()) == nullptr) {
      std::fprintf(stderr, "FAIL: cannot make a scratch directory %s\n",
                   name.c_str());
      std::abort();
    }
    path = name;
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  std::filesystem::path path;  ///< The directory.
};

/// @brief The file's bytes (@ref read_file).
/// @param path  The file.
/// @return Its bytes; none when it cannot be read.
inline std::vector<std::uint8_t> read_bytes(const std::string& path) {
  const Result<std::string> bytes = read_file(path);
  if (!bytes.ok()) return {};
  return {bytes->begin(), bytes->end()};
}

}  // namespace volumetric_kit::core::test
