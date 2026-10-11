// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/file_io.hpp"

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <ios>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

namespace fs = std::filesystem;
using Code = Status::Code;

constexpr mode_t kPermissionBits = 07777;

// Bytes no text API would round-trip: NULs, high bytes, and a length that is
// no multiple of read_file's 64 KiB first read.
std::string binary_bytes(std::size_t size) {
  std::string bytes(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<char>(((i * 131U) + 7U) % 256U);
  }
  return bytes;
}

mode_t permissions_of(const std::string& path) {
  struct stat info{};
  EXPECT_EQ(::stat(path.c_str(), &info), 0) << path;
  return info.st_mode & kPermissionBits;
}

bool is_symlink(const std::string& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

// The process umask, set for the scope.
class ScopedUmask {
 public:
  explicit ScopedUmask(mode_t mask) : old_(::umask(mask)) {}
  ~ScopedUmask() { ::umask(old_); }
  ScopedUmask(const ScopedUmask&) = delete;
  ScopedUmask& operator=(const ScopedUmask&) = delete;
  ScopedUmask(ScopedUmask&&) = delete;
  ScopedUmask& operator=(ScopedUmask&&) = delete;

 private:
  mode_t old_;
};

// A file-size limit for the scope: a write past it fails with EFBIG, as on a
// full disk, instead of raising SIGXFSZ. It works as root too.
class ScopedFileSizeLimit {
 public:
  explicit ScopedFileSizeLimit(rlim_t bytes)
      : old_handler_(std::signal(SIGXFSZ, SIG_IGN)) {
    EXPECT_EQ(::getrlimit(RLIMIT_FSIZE, &old_limit_), 0);
    rlimit limit = old_limit_;
    limit.rlim_cur = bytes;
    EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &limit), 0);
  }
  ~ScopedFileSizeLimit() {
    ::setrlimit(RLIMIT_FSIZE, &old_limit_);
    std::signal(SIGXFSZ, old_handler_);
  }
  ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
  ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;
  ScopedFileSizeLimit(ScopedFileSizeLimit&&) = delete;
  ScopedFileSizeLimit& operator=(ScopedFileSizeLimit&&) = delete;

 private:
  rlimit old_limit_{};
  void (*old_handler_)(int);
};

// Each test runs in a new temporary directory, removed afterwards.
class FileIo : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (fs::temp_directory_path() / "vkc_file_io_XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  // A path in the test's directory.
  std::string at(const std::string& name) const { return root_ + "/" + name; }

  // The names in `dir` (the test's directory by default), dot files and
  // temporary files included, sorted.
  std::vector<std::string> entries(const std::string& dir = {}) const {
    std::vector<std::string> names;
    for (const auto& entry :
         fs::directory_iterator(dir.empty() ? root_ : dir)) {
      names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  std::string root_;
};

std::string read_ok(const std::string& path) {
  Result<std::string> bytes = read_file(path);
  EXPECT_TRUE(bytes.ok()) << bytes.status().message();
  return bytes.ok() ? *std::move(bytes) : std::string();
}

TEST_F(FileIo, BytesRoundTripInEitherMode) {
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{1}, std::size_t{(1U << 20U) + 3U}}) {
    const std::string bytes = binary_bytes(size);
    const std::string replaced = at("replaced_" + std::to_string(size));
    const std::string created = at("created_" + std::to_string(size));
    ASSERT_TRUE(write_file(replaced, bytes, WriteMode::Replace));
    ASSERT_TRUE(write_file(created, bytes, WriteMode::CreateNew));
    EXPECT_EQ(read_ok(replaced), bytes) << size;
    EXPECT_EQ(read_ok(created), bytes) << size;
  }
}

TEST_F(FileIo, ReplaceReplacesAndLeavesNoTemporaryFile) {
  const std::string path = at("session.json");
  ASSERT_TRUE(write_file(path, "old, and longer", WriteMode::Replace));
  ASSERT_TRUE(write_file(path, "new", WriteMode::Replace));
  EXPECT_EQ(read_ok(path), "new");
  EXPECT_EQ(entries(), std::vector<std::string>{"session.json"});
}

TEST_F(FileIo, AFailedWriteLeavesWhatWasThereAndNoTemporaryFile) {
  const std::string path = at("session.json");
  ASSERT_TRUE(write_file(path, "old", WriteMode::Replace));
  Status replaced;
  Status created;
  {
    const ScopedFileSizeLimit limit(4096);
    const std::string big(std::size_t{1} << 16U, 'n');
    replaced = write_file(path, big, WriteMode::Replace);
    created = write_file(at("capture.json"), big, WriteMode::CreateNew);
  }
  EXPECT_EQ(replaced.domain(), Code::IoError);
  EXPECT_NE(replaced.message().find("write_file: cannot write " + path),
            std::string::npos)
      << replaced.message();
  EXPECT_EQ(created.domain(), Code::IoError);
  EXPECT_EQ(read_ok(path), "old");
  EXPECT_EQ(entries(), std::vector<std::string>{"session.json"});
}

TEST_F(FileIo, ReplaceKeepsThePermissions) {
  const std::string path = at("calibration.json");
  ASSERT_TRUE(write_file(path, "old", WriteMode::Replace));
  for (const mode_t mode : {mode_t{0640}, mode_t{0604}, mode_t{0444}}) {
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
    ASSERT_TRUE(write_file(path, "new", WriteMode::Replace));
    EXPECT_EQ(permissions_of(path), mode) << std::oct << mode;
  }
}

TEST_F(FileIo, ANewFileGetsTheUmasksMode) {
  for (const mode_t mask : {mode_t{022}, mode_t{027}}) {
    const ScopedUmask umask(mask);
    const std::string replaced = at("replaced_" + std::to_string(mask));
    const std::string created = at("created_" + std::to_string(mask));
    ASSERT_TRUE(write_file(replaced, "x", WriteMode::Replace));
    ASSERT_TRUE(write_file(created, "x", WriteMode::CreateNew));
    EXPECT_EQ(permissions_of(replaced), 0666U & ~mask) << std::oct << mask;
    EXPECT_EQ(permissions_of(created), 0666U & ~mask) << std::oct << mask;
  }
}

TEST_F(FileIo, ReplaceWritesThroughSymbolicLinks) {
  // links/session.json -> ../data/link.json -> real.json, both relative and in
  // other directories, so the temporary file must go beside real.json.
  ASSERT_TRUE(create_new_directory(at("links")));
  ASSERT_TRUE(create_new_directory(at("data")));
  ASSERT_TRUE(write_file(at("data/real.json"), "old", WriteMode::Replace));
  ASSERT_EQ(::symlink("real.json", at("data/link.json").c_str()), 0);
  ASSERT_EQ(::symlink("../data/link.json", at("links/session.json").c_str()),
            0);
  ASSERT_EQ(::chmod(at("data/real.json").c_str(), 0640), 0);

  ASSERT_TRUE(write_file(at("links/session.json"), "new", WriteMode::Replace));

  EXPECT_TRUE(is_symlink(at("links/session.json")));
  EXPECT_TRUE(is_symlink(at("data/link.json")));
  EXPECT_EQ(read_ok(at("data/real.json")), "new");
  EXPECT_EQ(permissions_of(at("data/real.json")), 0640U);
  EXPECT_EQ(entries(at("links")), std::vector<std::string>{"session.json"});
  EXPECT_EQ(entries(at("data")),
            (std::vector<std::string>{"link.json", "real.json"}));
}

TEST_F(FileIo, ReplaceThroughADanglingLinkCreatesItsTarget) {
  ASSERT_EQ(::symlink("target.json", at("link.json").c_str()), 0);
  ASSERT_TRUE(write_file(at("link.json"), "new", WriteMode::Replace));
  EXPECT_TRUE(is_symlink(at("link.json")));
  EXPECT_EQ(read_ok(at("target.json")), "new");
}

TEST_F(FileIo, ReplaceRefusesALinkLoop) {
  ASSERT_EQ(::symlink("b", at("a").c_str()), 0);
  ASSERT_EQ(::symlink("a", at("b").c_str()), 0);
  EXPECT_EQ(write_file(at("a"), "x", WriteMode::Replace).domain(),
            Code::IoError);
  EXPECT_EQ(entries(), (std::vector<std::string>{"a", "b"}));
}

TEST_F(FileIo, CreateNewRefusesAnythingThereAndLeavesNoTemporaryFile) {
  ASSERT_TRUE(write_file(at("img_000.jpg"), "first", WriteMode::CreateNew));
  ASSERT_EQ(::symlink("missing.jpg", at("dangling.jpg").c_str()), 0);
  ASSERT_TRUE(create_new_directory(at("images")));

  for (const char* name : {"img_000.jpg", "dangling.jpg", "images"}) {
    const Status s = write_file(at(name), "second", WriteMode::CreateNew);
    EXPECT_EQ(s.domain(), Code::InvalidArgument) << name;
    EXPECT_NE(s.message().find("write_file: cannot create " + at(name)),
              std::string::npos)
        << s.message();
  }
  EXPECT_EQ(read_ok(at("img_000.jpg")), "first");
  EXPECT_TRUE(is_symlink(at("dangling.jpg")));
  EXPECT_EQ(entries(), (std::vector<std::string>{"dangling.jpg", "images",
                                                 "img_000.jpg"}));
  EXPECT_TRUE(entries(at("images")).empty());
}

TEST_F(FileIo, ADirectoryOrAnotherNonRegularFileIsRefusedAsAFile) {
  ASSERT_TRUE(create_new_directory(at("results")));
  ASSERT_EQ(::symlink("results", at("latest").c_str()), 0);
  ASSERT_EQ(::mkfifo(at("pipe").c_str(), 0600), 0);

  EXPECT_EQ(read_file(at("results")).status().domain(), Code::InvalidArgument);
  for (const char* name : {"results", "latest", "pipe"}) {
    for (const WriteMode mode : {WriteMode::Replace, WriteMode::CreateNew}) {
      EXPECT_EQ(write_file(at(name), "x", mode).domain(), Code::InvalidArgument)
          << name;
    }
  }
  EXPECT_TRUE(fs::is_directory(at("results")));
  EXPECT_TRUE(fs::is_fifo(at("pipe")));
  EXPECT_TRUE(entries(at("results")).empty());
  EXPECT_EQ(entries(), (std::vector<std::string>{"latest", "pipe", "results"}));
}

TEST_F(FileIo, AMissingFileOrDirectoryIsNotFound) {
  const Result<std::string> missing = read_file(at("missing.json"));
  EXPECT_EQ(missing.status().domain(), Code::NotFound);
  EXPECT_NE(missing.status().message().find("read_file: cannot open " +
                                            at("missing.json") + ": "),
            std::string::npos)
      << missing.status().message();
  for (const WriteMode mode : {WriteMode::Replace, WriteMode::CreateNew}) {
    EXPECT_EQ(write_file(at("missing/a.json"), "x", mode).domain(),
              Code::NotFound);
  }
  EXPECT_TRUE(entries().empty());
}

// Writers in two threads and in two processes, replacing one file at once:
// each gets a temporary file of its own, so every write succeeds whole.
TEST_F(FileIo, ConcurrentWritersGetDistinctTemporaryFiles) {
  const std::string path = at("session.json");
  constexpr int kWrites = 40;
  constexpr std::size_t kSize = std::size_t{1} << 14U;
  const auto writer = [&](char fill) {
    int failures = 0;
    const std::string bytes(kSize, fill);
    for (int i = 0; i < kWrites; ++i) {
      if (!write_file(path, bytes, WriteMode::Replace)) ++failures;
    }
    return failures;
  };

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) _exit(writer('c') == 0 ? 0 : 1);
  int a_failures = 0;
  int b_failures = 0;
  std::thread a([&] { a_failures = writer('a'); });
  std::thread b([&] { b_failures = writer('b'); });
  a.join();
  b.join();
  int child_status = 0;
  ASSERT_EQ(::waitpid(child, &child_status, 0), child);

  EXPECT_EQ(a_failures, 0);
  EXPECT_EQ(b_failures, 0);
  EXPECT_TRUE(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  const std::string last = read_ok(path);
  ASSERT_EQ(last.size(), kSize);
  EXPECT_EQ(std::count(last.begin(), last.end(), last.front()),
            static_cast<std::ptrdiff_t>(kSize));
  EXPECT_EQ(entries(), std::vector<std::string>{"session.json"});
}

TEST_F(FileIo, CreateNewDirectoryRefusesAnythingThere) {
  ASSERT_TRUE(create_new_directory(at("session")));
  EXPECT_TRUE(fs::is_directory(at("session")));
  ASSERT_TRUE(write_file(at("file"), "x", WriteMode::CreateNew));

  for (const char* name : {"session", "file"}) {
    const Status s = create_new_directory(at(name));
    EXPECT_EQ(s.domain(), Code::InvalidArgument) << name;
    EXPECT_NE(
        s.message().find("create_new_directory: cannot create " + at(name)),
        std::string::npos)
        << s.message();
  }
  EXPECT_EQ(create_new_directory(at("missing/session")).domain(),
            Code::NotFound);
}

TEST_F(FileIo, ANewDirectoryGetsTheUmasksMode) {
  const ScopedUmask umask(027);
  ASSERT_TRUE(create_new_directory(at("session")));
  EXPECT_EQ(permissions_of(at("session")), 0750U);
}

TEST(FileIoPaths, AnEmptyPathOrAnEmbeddedNulIsRefused) {
  const std::string nul("dir/a\0b.json", 12);
  for (const std::string& path : {std::string(), nul}) {
    const Result<std::string> read = read_file(path);
    EXPECT_EQ(read.status().domain(), Code::InvalidArgument);
    EXPECT_EQ(read.status().message().rfind("read_file: ", 0), 0U);
    for (const WriteMode mode : {WriteMode::Replace, WriteMode::CreateNew}) {
      EXPECT_EQ(write_file(path, "x", mode).domain(), Code::InvalidArgument);
    }
    EXPECT_EQ(create_new_directory(path).domain(), Code::InvalidArgument);
  }
}

}  // namespace
}  // namespace volumetric_kit::core
