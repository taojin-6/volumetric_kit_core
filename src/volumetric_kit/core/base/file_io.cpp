// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/file_io.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"

namespace volumetric_kit::core {
namespace {

// One read() or write() moves at most this much: macOS refuses a count above
// INT_MAX.
constexpr std::size_t kMaxIo = std::size_t{1} << 30;
// What read_file reads first when the size is unknown, and the least its
// buffer grows by.
constexpr std::size_t kChunk = std::size_t{1} << 16;
// Symbolic links followed from a written path, as Linux's MAXSYMLINKS.
constexpr int kMaxLinks = 40;
// Temporary names tried before giving up; each taken one moves to the next.
constexpr int kMaxTempNames = 100;

// The permission bits a replaced file keeps (07777), and what a new file or
// directory is created with before the umask (0666, 0777).
constexpr mode_t kPermissionBits =
    S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO;
constexpr mode_t kNewFileMode =
    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
constexpr mode_t kNewDirectoryMode = S_IRWXU | S_IRWXG | S_IRWXO;

// "<api>: <what> <path>: <the system's reason>", in the domain `error` says.
Status system_failure(std::string_view api, std::string_view what,
                      std::string_view path, int error) {
  std::string message(api);
  message += ": ";
  message += what;
  message += ' ';
  message += path;
  message += ": ";
  message += std::generic_category().message(error);
  switch (error) {
    case ENOENT:
      return Status::not_found(std::move(message));
    case EEXIST:
    case EISDIR:
      return Status::invalid_argument(std::move(message));
    case ENOMEM:
      return Status::out_of_memory(std::move(message));
    default:
      return Status::io_error(std::move(message));
  }
}

Status check_path(std::string_view api, const std::string& path) {
  if (path.empty()) {
    return Status::invalid_argument(std::string(api) + ": the path is empty");
  }
  if (path.find('\0') != std::string::npos) {
    return Status::invalid_argument(std::string(api) +
                                    ": the path has an embedded NUL");
  }
  return {};
}

// The directory part of `path` with its trailing '/', or "" for a bare name,
// so the part plus a name is a path in the same directory.
std::string directory_of(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Flush a directory's entries, where the filesystem allows it. Best effort:
// the rename or link it follows has already happened.
void sync_directory(const std::string& directory) {
  const UniqueFd dir(::open(directory.empty() ? "." : directory.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.get() >= 0) (void)::fsync(dir.get());
}

// A temporary file that write_file fills, then renames or links into place.
// Until then, destruction removes it.
class TempFile {
 public:
  TempFile() = default;
  ~TempFile() {
    if (!path_.empty()) ::unlink(path_.c_str());
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;

  // Create it in `directory` (as directory_of returns it) as
  // ".vkc_tmp.<pid>.<n>". O_EXCL keeps two writers off one name, and a taken
  // name -- another writer's, or one a crashed writer left -- moves to the
  // next; the process id and the counter make that rare.
  Status create(std::string_view api, const std::string& directory) {
    static std::atomic<std::uint64_t> counter{0};
    const std::string prefix =
        directory + ".vkc_tmp." + std::to_string(::getpid()) + ".";
    for (int attempt = 0; attempt < kMaxTempNames; ++attempt) {
      std::string name = prefix + std::to_string(counter.fetch_add(
                                      1, std::memory_order_relaxed));
      const int fd = ::open(
          name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, kNewFileMode);
      if (fd >= 0) {
        fd_.reset(fd);
        path_ = std::move(name);
        return {};
      }
      const int error = errno;
      if (error != EEXIST && error != EINTR) {
        return system_failure(api, "cannot create a temporary file in",
                              directory.empty() ? "." : directory, error);
      }
    }
    return Status::io_error(std::string(api) + ": no free temporary name in " +
                            (directory.empty() ? "." : directory));
  }

  // Write `bytes`, set `mode` if given, flush and close. `path` names the
  // destination in a failure's message.
  Status finish(std::string_view api, const std::string& path,
                std::string_view bytes, std::optional<mode_t> mode) {
    while (!bytes.empty()) {
      const ssize_t n =
          ::write(fd_.get(), bytes.data(), std::min(bytes.size(), kMaxIo));
      if (n <= 0) {
        const int error = n == 0 ? EIO : errno;
        if (error == EINTR) continue;
        return system_failure(api, "cannot write", path, error);
      }
      bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    if (mode && ::fchmod(fd_.get(), *mode) != 0) {
      return system_failure(api, "cannot set the permissions of", path, errno);
    }
    if (::fsync(fd_.get()) != 0) {
      return system_failure(api, "cannot flush", path, errno);
    }
    if (::close(fd_.release()) != 0 && errno != EINTR) {
      return system_failure(api, "cannot close", path, errno);
    }
    return {};
  }

  const std::string& path() const noexcept { return path_; }

  // The file was renamed into place: its temporary name is gone.
  void renamed() noexcept { path_.clear(); }

  // Remove the temporary name, once the file has another.
  void remove() noexcept {
    ::unlink(path_.c_str());
    path_.clear();
  }

 private:
  UniqueFd fd_;
  std::string path_;
};

Result<std::string> read_link(std::string_view api, const std::string& path) {
  std::string target(256, '\0');
  for (;;) {
    const ssize_t n = ::readlink(path.c_str(), target.data(), target.size());
    if (n < 0) {
      return system_failure(api, "cannot read the link", path, errno);
    }
    if (static_cast<std::size_t>(n) < target.size()) {
      target.resize(static_cast<std::size_t>(n));
      return target;
    }
    target.resize(target.size() * 2);
  }
}

// The file a write to `path` replaces: `path`, or what the symbolic links at
// it lead to, or where a dangling one points.
Result<std::string> resolve_links(std::string_view api,
                                  const std::string& path) {
  std::string at = path;
  for (int followed = 0; followed <= kMaxLinks; ++followed) {
    struct stat info{};
    if (::lstat(at.c_str(), &info) != 0) {
      const int error = errno;
      if (error == ENOENT) return at;
      return system_failure(api, "cannot inspect", at, error);
    }
    if (!S_ISLNK(info.st_mode)) return at;
    VKC_ASSIGN(std::string target, read_link(api, at));
    if (target.empty() || target.front() != '/') {
      target.insert(0, directory_of(at));
    }
    at = std::move(target);
  }
  return system_failure(api, "cannot follow the links at", path, ELOOP);
}

Result<std::string> read_whole(const std::string& path) {
  constexpr std::string_view kApi = "read_file";
  const UniqueFd in(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (in.get() < 0) return system_failure(kApi, "cannot open", path, errno);
  struct stat info{};
  if (::fstat(in.get(), &info) != 0) {
    return system_failure(kApi, "cannot inspect", path, errno);
  }
  // A regular file's size, plus one byte for the read that finds the end; a
  // file of unknown size (a pipe, procfs) grows the buffer as it is read.
  std::string bytes(S_ISREG(info.st_mode) && info.st_size > 0
                        ? static_cast<std::size_t>(info.st_size) + 1
                        : kChunk,
                    '\0');
  std::size_t used = 0;
  for (;;) {
    if (used == bytes.size()) {
      bytes.resize(bytes.size() + std::max(bytes.size(), kChunk));
    }
    const ssize_t n =
        ::read(in.get(), &bytes[used], std::min(bytes.size() - used, kMaxIo));
    if (n < 0) {
      const int error = errno;
      if (error == EINTR) continue;
      return system_failure(kApi, "cannot read", path, error);
    }
    if (n == 0) break;
    used += static_cast<std::size_t>(n);
  }
  bytes.resize(used);
  return bytes;
}

Status replace_file(const std::string& path, std::string_view bytes) {
  constexpr std::string_view kApi = "write_file";
  VKC_ASSIGN(const std::string target, resolve_links(kApi, path));
  std::optional<mode_t> mode;
  struct stat info{};
  if (::stat(target.c_str(), &info) == 0) {
    if (S_ISDIR(info.st_mode)) {
      return system_failure(kApi, "cannot replace", path, EISDIR);
    }
    if (!S_ISREG(info.st_mode)) {
      return Status::invalid_argument(std::string(kApi) + ": cannot replace " +
                                      path + ": not a regular file");
    }
    mode = info.st_mode & kPermissionBits;
  } else if (errno != ENOENT) {
    return system_failure(kApi, "cannot inspect", path, errno);
  }
  const std::string directory = directory_of(target);
  TempFile temp;
  VKC_TRY(temp.create(kApi, directory));
  VKC_TRY(temp.finish(kApi, path, bytes, mode));
  if (::rename(temp.path().c_str(), target.c_str()) != 0) {
    return system_failure(kApi, "cannot replace", path, errno);
  }
  temp.renamed();
  sync_directory(directory);
  return {};
}

Status create_file(const std::string& path, std::string_view bytes) {
  constexpr std::string_view kApi = "write_file";
  const std::string directory = directory_of(path);
  TempFile temp;
  VKC_TRY(temp.create(kApi, directory));
  VKC_TRY(temp.finish(kApi, path, bytes, std::nullopt));
  // link() refuses whatever is at `path`, a dangling symbolic link included,
  // and the file appears there already complete.
  if (::link(temp.path().c_str(), path.c_str()) != 0) {
    return system_failure(kApi, "cannot create", path, errno);
  }
  temp.remove();
  sync_directory(directory);
  return {};
}

}  // namespace

Result<std::string> read_file(const std::string& path) {
  constexpr std::string_view kApi = "read_file";
  try {
    VKC_TRY(check_path(kApi, path));
    return read_whole(path);
  } catch (...) {
    return exception_status(kApi, Status::Code::IoError);
  }
}

Status write_file(const std::string& path, std::string_view bytes,
                  WriteMode mode) {
  constexpr std::string_view kApi = "write_file";
  try {
    VKC_TRY(check_path(kApi, path));
    return mode == WriteMode::Replace ? replace_file(path, bytes)
                                      : create_file(path, bytes);
  } catch (...) {
    return exception_status(kApi, Status::Code::IoError);
  }
}

Status create_new_directory(const std::string& path) {
  constexpr std::string_view kApi = "create_new_directory";
  try {
    VKC_TRY(check_path(kApi, path));
    if (::mkdir(path.c_str(), kNewDirectoryMode) != 0) {
      return system_failure(kApi, "cannot create", path, errno);
    }
    return {};
  } catch (...) {
    return exception_status(kApi, Status::Code::IoError);
  }
}

}  // namespace volumetric_kit::core
