// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/file_io.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>  // IWYU pragma: keep (mode_t, pid_t, ssize_t on Linux)
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"

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

// A signal ignored for the scope.
class ScopedIgnoredSignal {
 public:
  explicit ScopedIgnoredSignal(int signal)
      : signal_(signal), old_handler_(std::signal(signal, SIG_IGN)) {}
  ~ScopedIgnoredSignal() { std::signal(signal_, old_handler_); }
  ScopedIgnoredSignal(const ScopedIgnoredSignal&) = delete;
  ScopedIgnoredSignal& operator=(const ScopedIgnoredSignal&) = delete;
  ScopedIgnoredSignal(ScopedIgnoredSignal&&) = delete;
  ScopedIgnoredSignal& operator=(ScopedIgnoredSignal&&) = delete;

 private:
  int signal_;
  void (*old_handler_)(int);
};

// A file-size limit for the scope: a write past it fails with EFBIG, as on a
// full disk, instead of raising SIGXFSZ. It works as root too.
class ScopedFileSizeLimit {
 public:
  explicit ScopedFileSizeLimit(rlim_t bytes) {
    EXPECT_EQ(::getrlimit(RLIMIT_FSIZE, &old_limit_), 0);
    rlimit limit = old_limit_;
    limit.rlim_cur = bytes;
    EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &limit), 0);
  }
  ~ScopedFileSizeLimit() { ::setrlimit(RLIMIT_FSIZE, &old_limit_); }
  ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
  ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;
  ScopedFileSizeLimit(ScopedFileSizeLimit&&) = delete;
  ScopedFileSizeLimit& operator=(ScopedFileSizeLimit&&) = delete;

 private:
  const ScopedIgnoredSignal sigxfsz_{SIGXFSZ};
  rlimit old_limit_{};
};

// A path's permission bits, changed for the scope.
class ScopedPermissions {
 public:
  ScopedPermissions(std::string path, mode_t mode)
      : path_(std::move(path)), old_(permissions_of(path_)) {
    EXPECT_EQ(::chmod(path_.c_str(), mode), 0) << path_;
  }
  ~ScopedPermissions() { ::chmod(path_.c_str(), old_); }
  ScopedPermissions(const ScopedPermissions&) = delete;
  ScopedPermissions& operator=(const ScopedPermissions&) = delete;
  ScopedPermissions(ScopedPermissions&&) = delete;
  ScopedPermissions& operator=(ScopedPermissions&&) = delete;

 private:
  std::string path_;
  mode_t old_;
};

// Lists a directory in a thread of its own until stopped, recording each
// temporary file write_file makes there (".vkc_tmp.*") with every permission
// bit it was seen with, so a write in the meantime is observed from outside.
class TemporaryFileWatcher {
 public:
  explicit TemporaryFileWatcher(std::string dir) : dir_(std::move(dir)) {}
  ~TemporaryFileWatcher() { join(); }
  TemporaryFileWatcher(const TemporaryFileWatcher&) = delete;
  TemporaryFileWatcher& operator=(const TemporaryFileWatcher&) = delete;
  TemporaryFileWatcher(TemporaryFileWatcher&&) = delete;
  TemporaryFileWatcher& operator=(TemporaryFileWatcher&&) = delete;

  // How many sightings so far, of any temporary file.
  int sightings() const { return sightings_.load(); }

  // Stop, and return each name seen with the union of its modes.
  std::map<std::string, mode_t> stop() {
    join();
    return seen_;
  }

 private:
  void join() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  void watch() {
    while (!stop_) {
      std::error_code error;
      for (fs::directory_iterator it(dir_, error), end; !error && it != end;
           it.increment(error)) {
        const std::string name = it->path().filename().string();
        struct stat info{};
        if (name.rfind(".vkc_tmp.", 0) != 0 ||
            ::lstat(it->path().c_str(), &info) != 0) {
          continue;  // not a temporary file, or renamed since it was listed
        }
        seen_[name] |= info.st_mode & kPermissionBits;
        ++sightings_;
      }
    }
  }

  std::string dir_;
  std::map<std::string, mode_t> seen_;
  std::atomic<int> sightings_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_{[this] { watch(); }};  // last: starts once all is set
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
  // 04755: set-user-ID, which an owner may set; macOS refuses the sticky bit
  // on a file.
  for (const mode_t mode :
       {mode_t{0640}, mode_t{0604}, mode_t{0444}, mode_t{04755}}) {
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
    ASSERT_TRUE(write_file(path, "new", WriteMode::Replace));
    EXPECT_EQ(permissions_of(path), mode) << std::oct << mode;
  }
}

// Under umask 022 a new file is 0644, so a temporary file made that way would
// let anyone open a 0600 file's new bytes while they are written.
TEST_F(FileIo, ReplaceKeepsAPrivateFilesNewBytesPrivate) {
  const ScopedUmask umask(022);
  const std::string path = at("private.json");
  ASSERT_TRUE(write_file(path, "old", WriteMode::Replace));
  ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
  const std::string big(std::size_t{16} << 20U, 'n');

  TemporaryFileWatcher watcher(root_);
  for (int i = 0; i < 50 && watcher.sightings() == 0; ++i) {
    ASSERT_TRUE(write_file(path, big, WriteMode::Replace));
  }
  const std::map<std::string, mode_t> seen = watcher.stop();

  ASSERT_FALSE(seen.empty()) << "no temporary file was seen";
  for (const auto& [name, mode] : seen) {
    EXPECT_EQ(mode & 077U, 0U) << name << ' ' << std::oct << mode;
  }
  EXPECT_EQ(permissions_of(path), 0600U);
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
  // other directories, and links/absolute.json -> <root>/data/real.json. The
  // temporary file must go beside real.json: links/ is read-only.
  ASSERT_TRUE(create_new_directory(at("links")));
  ASSERT_TRUE(create_new_directory(at("data")));
  ASSERT_TRUE(write_file(at("data/real.json"), "old", WriteMode::Replace));
  ASSERT_EQ(::symlink("real.json", at("data/link.json").c_str()), 0);
  ASSERT_EQ(::symlink("../data/link.json", at("links/session.json").c_str()),
            0);
  ASSERT_EQ(::symlink(at("data/real.json").c_str(),
                      at("links/absolute.json").c_str()),
            0);
  ASSERT_EQ(::chmod(at("data/real.json").c_str(), 0640), 0);

  {
    const ScopedPermissions read_only(at("links"), 0555);
    ASSERT_TRUE(
        write_file(at("links/session.json"), "new", WriteMode::Replace));
    EXPECT_EQ(read_ok(at("data/real.json")), "new");
    ASSERT_TRUE(
        write_file(at("links/absolute.json"), "newer", WriteMode::Replace));
    EXPECT_EQ(read_ok(at("data/real.json")), "newer");
  }

  EXPECT_TRUE(is_symlink(at("links/session.json")));
  EXPECT_TRUE(is_symlink(at("links/absolute.json")));
  EXPECT_TRUE(is_symlink(at("data/link.json")));
  EXPECT_EQ(permissions_of(at("data/real.json")), 0640U);
  EXPECT_EQ(entries(at("links")),
            (std::vector<std::string>{"absolute.json", "session.json"}));
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
    const Status s = write_file(at("missing/a.json"), "x", mode);
    EXPECT_EQ(s.domain(), Code::NotFound);
    // The message names the file asked for, not only its directory.
    EXPECT_NE(s.message().find("write_file: cannot create a temporary file "
                               "for " +
                               at("missing/a.json") + " in "),
              std::string::npos)
        << s.message();
  }
  EXPECT_TRUE(entries().empty());
}

// A component of the path that is a file, not a directory: the directory the
// path names does not exist either.
TEST_F(FileIo, AFileWhereADirectoryShouldBeIsNotFound) {
  ASSERT_TRUE(write_file(at("plain"), "x", WriteMode::CreateNew));
  EXPECT_EQ(read_file(at("plain/a.json")).status().domain(), Code::NotFound);
  for (const WriteMode mode : {WriteMode::Replace, WriteMode::CreateNew}) {
    EXPECT_EQ(write_file(at("plain/a.json"), "x", mode).domain(),
              Code::NotFound);
  }
  EXPECT_EQ(create_new_directory(at("plain/session")).domain(), Code::NotFound);
  EXPECT_EQ(entries(), std::vector<std::string>{"plain"});
}

// A file of unknown size, as a pipe is, is read to its end however long it
// is: past read_file's 64 KiB first read.
TEST_F(FileIo, AFileOfUnknownSizeIsReadWhole) {
  const std::string pipe = at("pipe");
  ASSERT_EQ(::mkfifo(pipe.c_str(), 0600), 0);
  const std::string bytes = binary_bytes((std::size_t{256} << 10U) + 5U);
  // A reader that stops early then fails the writer (EPIPE) instead of
  // killing the test.
  const ScopedIgnoredSignal sigpipe(SIGPIPE);
  std::thread writer([&] {
    const UniqueFd out(::open(pipe.c_str(), O_WRONLY | O_CLOEXEC));
    std::string_view rest = bytes;
    while (out.valid() && !rest.empty()) {
      const ssize_t n = ::write(out.get(), rest.data(), rest.size());
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      rest.remove_prefix(static_cast<std::size_t>(n));
    }
  });
  const Result<std::string> read = read_file(pipe);
  writer.join();
  ASSERT_TRUE(read.ok()) << read.status().message();
  EXPECT_EQ(read->size(), bytes.size());
  EXPECT_TRUE(*read == bytes);
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
  EXPECT_EQ(child_status, 0);  // exited, with status 0
  const std::string last = read_ok(path);
  ASSERT_EQ(last.size(), kSize);
  EXPECT_EQ(std::count(last.begin(), last.end(), last.front()),
            static_cast<std::ptrdiff_t>(kSize));
  EXPECT_EQ(entries(), std::vector<std::string>{"session.json"});
}

// Temporary names already taken -- by files a crashed writer left, or by
// symbolic links to a file elsewhere -- are skipped and left as they are.
TEST_F(FileIo, TakenTemporaryNamesAreSkippedAndLeftAlone) {
  ASSERT_TRUE(create_new_directory(at("dir")));
  ASSERT_TRUE(
      write_file(at("elsewhere.json"), "elsewhere", WriteMode::Replace));
  const std::string path = at("dir/session.json");
  const std::string prefix = ".vkc_tmp." + std::to_string(::getpid()) + ".";

  // The temporary names are ".vkc_tmp.<pid>.<n>", n counting up in the
  // process: watch a write to learn a recent n.
  std::uint64_t last = 0;
  {
    TemporaryFileWatcher watcher(at("dir"));
    const std::string big(std::size_t{16} << 20U, 'n');
    for (int i = 0; i < 50 && watcher.sightings() == 0; ++i) {
      ASSERT_TRUE(write_file(path, big, WriteMode::Replace));
    }
    const std::map<std::string, mode_t> seen = watcher.stop();
    ASSERT_FALSE(seen.empty()) << "no temporary file was seen";
    for (const auto& entry : seen) {
      ASSERT_EQ(entry.first.rfind(prefix, 0), 0U) << entry.first;
      last = std::max<std::uint64_t>(
          last, std::stoull(entry.first.substr(prefix.size())));
    }
  }
  // Take the next ten, alternately with a file and a link, without
  // write_file, which would use a name itself. The watcher may have missed
  // the last write's name, so the next write starts at the first or second.
  std::vector<std::string> expected = {"session.json"};
  for (std::uint64_t n = last + 1; n <= last + 10; ++n) {
    const std::string name = prefix + std::to_string(n);
    if (n % 2 == 0) {
      std::ofstream(at("dir/" + name)) << "left";
    } else {
      ASSERT_EQ(
          ::symlink(at("elsewhere.json").c_str(), at("dir/" + name).c_str()),
          0);
    }
    expected.push_back(name);
  }
  std::sort(expected.begin(), expected.end());

  ASSERT_TRUE(write_file(path, "new", WriteMode::Replace));

  EXPECT_EQ(read_ok(path), "new");
  EXPECT_FALSE(is_symlink(path));
  EXPECT_EQ(entries(at("dir")), expected);
  for (std::uint64_t n = last + 1; n <= last + 10; ++n) {
    const std::string planted = at("dir/" + prefix + std::to_string(n));
    if (n % 2 == 0) {
      EXPECT_EQ(read_ok(planted), "left") << n;
    } else {
      EXPECT_TRUE(is_symlink(planted)) << n;
    }
  }
  EXPECT_EQ(read_ok(at("elsewhere.json")), "elsewhere");
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
  EXPECT_TRUE(check_path("load_mesh", "dir/a.json"));
  EXPECT_EQ(check_path("load_mesh", "").message(),
            "load_mesh: the path is empty");
  EXPECT_EQ(check_path("load_mesh", nul).message(),
            "load_mesh: the path has an embedded NUL");
  for (const std::string& path : {std::string(), nul}) {
    EXPECT_EQ(check_path("load_mesh", path).domain(), Code::InvalidArgument);
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
