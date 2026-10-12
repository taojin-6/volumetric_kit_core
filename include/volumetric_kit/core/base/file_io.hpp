// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file file_io.hpp
/// @brief Whole-file reads and writes, and a new directory, that report
///        failure as a `Status`.
///
/// A write never leaves a partial file at its path: the bytes go to a
/// temporary file beside the destination, are flushed to the storage device
/// (`fsync`; on Apple `F_FULLFSYNC`, as `fsync` there stops at the drive's
/// cache), and only then take the destination's name. @ref WriteMode says
/// whether they may replace a file already there.
///
/// Paths are POSIX paths, relative to the working directory or absolute. An
/// empty path, or one with an embedded NUL, is refused before any file
/// access (@ref check_path). A failure's domain is:
/// - `NotFound`: the file, or a directory on its path, does not exist (a
///   component that is a file, not a directory, included);
/// - `InvalidArgument`: the path is refused, names a directory or other
///   non-regular file where a regular file is meant, or names something that
///   exists where a new file or directory is asked for;
/// - `OutOfMemory`: an allocation failed;
/// - `IoError`: any other failure the system reports (a full disk, a denied
///   permission, ...).
///
/// The message names the function, the path and the system's reason.

#include <string>
#include <string_view>

#include "volumetric_kit/core/base/export.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {

/// @brief Refuse a path no file call can take: an empty one, or one with an
///        embedded NUL, which a C API would cut short.
/// @param api   The caller, which starts the message.
/// @param path  The path to check.
/// @return Success, or `InvalidArgument` naming @p api and the reason.
///
/// @code
/// VKC_TRY(check_path("load_mesh", path));  // before handing it to a library
/// @endcode
VKC_BASE_API Status check_path(std::string_view api, const std::string& path);

/// @brief What @ref write_file does when something is already at its path.
///
/// @code
/// VKC_TRY(write_file(dir + "/session.json", json, WriteMode::Replace));
/// VKC_TRY(write_file(dir + "/img_000.jpg", jpeg, WriteMode::CreateNew));
/// @endcode
enum class WriteMode {
  /// Replace the file atomically, or create it: a reader, or a crash, finds
  /// the old bytes or the new ones, never a mix. The replaced file's
  /// permission bits are kept, and until they are set only the writer can
  /// open the new bytes; a new file gets `0666` less the umask. A symbolic
  /// link at the path stays a link, and the file it points to is replaced.
  /// The file is a new one: other hard links to the old one keep the old
  /// bytes, and its owner, ACLs and extended attributes are not kept.
  Replace,
  /// Create the file only if nothing is at the path, not even a symbolic
  /// link; otherwise refuse (`InvalidArgument`) and leave what is there
  /// untouched. The file appears complete, as a hard link to the temporary
  /// file, so the filesystem must have hard links; it gets `0666` less the
  /// umask.
  CreateNew,
};

/// @brief Read a whole file.
/// @param path  The file to read; a symbolic link is followed.
/// @return Its bytes, which may include NULs; or `NotFound` when it does not
///         exist, `InvalidArgument` for a directory, `IoError` when it cannot
///         be opened or read.
///
/// @code
/// VKC_ASSIGN(const std::string text, read_file("rig/calibration.json"));
/// @endcode
VKC_BASE_API Result<std::string> read_file(const std::string& path);

/// @brief Write a whole file, never leaving a partial one at @p path.
///
/// The temporary file, `.vkc_tmp.<pid>.<n>`, is created exclusively, so no
/// other writer, in this process or another, uses it at the same time; on
/// failure it is removed, but a crash can leave it behind. After the rename
/// or link, the directory is flushed too, where the filesystem allows it.
///
/// @param path   The file to write.
/// @param bytes  The whole content.
/// @param mode   Whether an existing file is replaced (@ref WriteMode).
/// @return Success once the bytes are flushed and at @p path. On failure,
///         what was at @p path is unchanged.
///
/// @code
/// VKC_TRY(write_file("captures/000.json", text, WriteMode::CreateNew));
/// @endcode
VKC_BASE_API Status write_file(const std::string& path, std::string_view bytes,
                               WriteMode mode);

/// @brief Create a directory that does not exist yet.
/// @param path  The directory to create, with `0777` less the umask. Its
///              parent must exist.
/// @return Success once it is created; `InvalidArgument` when anything is
///         already at @p path, a directory included; `NotFound` when its
///         parent does not exist.
///
/// @code
/// VKC_TRY(create_new_directory(out + "/2026-10-11_143000"));
/// @endcode
VKC_BASE_API Status create_new_directory(const std::string& path);

}  // namespace volumetric_kit::core
