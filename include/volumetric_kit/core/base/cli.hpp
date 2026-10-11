// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file cli.hpp
/// @brief The family's command-line parser.
///
/// Each flag is declared once, with the variable it sets and the name of its
/// value, and the usage line is made from those declarations, so it cannot
/// leave a flag out. A number is read whole and finite: `10x`, ` 5`, `nan`
/// and an empty value are refused, naming the flag.
///
/// @code
/// int main(int argc, char** argv) {
///   std::string scene_dir, out = "out.ply";
///   int stride = 1;
///   bool preload = false;
///   Cli cli("fuse_replica");
///   cli.positional("scene_dir", scene_dir)
///       .option({"-o", "--out"}, "out.ply", out)
///       .option("--stride", "N", stride, /*min=*/1)
///       .flag("--preload", preload);
///   if (const Status s = cli.parse(argc, argv); !s) {
///     std::fprintf(stderr, "%s\n", s.message().c_str());  // with the usage
///     return 2;
///   }
///   ...
/// }
/// @endcode

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/export.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {

/// @brief Parse @p text, the value of @p flag, as a finite number: the whole
///        text, so `10x`, ` 5`, `nan` and an empty value are refused. A value
///        too small for a double rounds toward zero.
/// @param flag  The flag's spelling, for the message.
/// @param text  The value; not null.
/// @param out   Set on success; unchanged on error.
/// @return OK, or InvalidArgument naming @p flag.
VKC_BASE_API Status parse_number(const std::string& flag, const char* text,
                                 double& out);

/// @brief As the double overload, also refusing a value past float's range.
/// @param flag  The flag's spelling, for the message.
/// @param text  The value; not null.
/// @param out   Set on success; unchanged on error.
/// @return OK, or InvalidArgument naming @p flag.
VKC_BASE_API Status parse_number(const std::string& flag, const char* text,
                                 float& out);

/// @brief Parse @p text, the value of @p flag, as a decimal `int`: the whole
///        text, in range.
/// @param flag  The flag's spelling, for the message.
/// @param text  The value; not null.
/// @param out   Set on success; unchanged on error.
/// @return OK, or InvalidArgument naming @p flag.
VKC_BASE_API Status parse_number(const std::string& flag, const char* text,
                                 int& out);

/// @brief Parse @p text, the value of @p flag, as a decimal count: digits
///        only, so a sign is refused as white space is.
/// @param flag  The flag's spelling, for the message.
/// @param text  The value; not null.
/// @param out   Set on success; unchanged on error.
/// @return OK, or InvalidArgument naming @p flag.
VKC_BASE_API Status parse_number(const std::string& flag, const char* text,
                                 std::uint32_t& out);

/// @brief A command line's flags and positional arguments, declared with the
///        variables they set, parsed in one pass.
///
/// A flag that takes a value takes the next argument whatever it looks like,
/// so `--up-axis -y` works; a flag given twice keeps the last. An argument
/// starting with `-` that names no flag is refused, as are a positional
/// argument past those declared, a second flag of a @ref one_of group, and a
/// command line without a positional or a @ref require "required" flag.
/// After the arguments, the @ref check "checks" run in declaration order
/// until one refuses. Every error is InvalidArgument, its message
/// `<program>: <what>` followed by the @ref usage line, for a `main` to print
/// as it is.
///
/// A declaration keeps a reference to its variable, which must outlive every
/// @ref parse. Declaring a spelling twice, or naming an undeclared flag in
/// @ref require or @ref one_of, is a programmer error (`VKC_CHECK`).
///
/// @code
/// std::string serial, rig;
/// bool hevc = true;
/// Cli cli("capture");
/// cli.option("--serial", "SN", serial)
///     .option("--rig", "sync.json", rig)
///     .one_of({"--serial", "--rig"})
///     .require("--rig")
///     .choice<bool>({{"--hevc", true}, {"--mjpeg", false}}, hevc);
/// // usage: capture (--serial SN | --rig sync.json) [--hevc | --mjpeg]
/// VKC_TRY(cli.parse(argc, argv));
/// @endcode
class VKC_BASE_API Cli {
 public:
  /// @brief A flag's spellings: one (`"--voxel"`) or several
  ///        (`{"-o", "--out"}`), the first shown in the usage line.
  ///
  /// @code
  /// cli.flag("--fast", fast).option({"-o", "--out"}, "path", out);
  /// @endcode
  struct Names {
    /// @brief One spelling, so a flag with one is written as a string.
    /// @param name  The spelling.
    // NOLINTNEXTLINE(google-explicit-constructor): `cli.flag("--x", ...)`.
    Names(const char* name) : list{name} {}
    /// @brief Several spellings, the first shown in the usage line.
    /// @param names  The spellings.
    Names(std::initializer_list<const char*> names)
        : list(names.begin(), names.end()) {}
    /// The spellings, in declaration order.
    std::vector<std::string> list;
  };

  /// @brief What a flag does with its value (null for a switch), given the
  ///        spelling it was found under; its refusal is the parse's.
  using Handler =
      std::function<Status(const std::string& flag, const char* value)>;

  /// @brief An empty command line for @p program.
  /// @param program  The program's name, for the usage line and errors.
  explicit Cli(std::string program);

  /// @brief A switch: @p out becomes @p value wherever it is given.
  /// @param names  The switch's spellings.
  /// @param out    The variable it sets.
  /// @param value  What it sets @p out to.
  /// @return This parser, for chaining.
  Cli& flag(Names names, bool& out, bool value = true);

  /// @brief A flag whose value is parsed as @p out's type: a number by
  ///        @ref parse_number, a `std::string` as given, a
  ///        `std::optional<float>` set from a number.
  /// @param names    The flag's spellings.
  /// @param metavar  The value's name in the usage line.
  /// @param out      The variable it sets.
  /// @return This parser, for chaining.
  template <typename T>
  Cli& option(Names names, const char* metavar, T& out) {
    return on(std::move(names), metavar,
              [&out](const std::string& flag, const char* value) {
                return assign(flag, value, out);
              });
  }

  /// @brief An integer flag refusing a value below @p min.
  /// @param names    The flag's spellings.
  /// @param metavar  The value's name in the usage line.
  /// @param out      The variable it sets.
  /// @param min      The smallest value accepted.
  /// @return This parser, for chaining.
  Cli& option(Names names, const char* metavar, int& out, int min);

  /// @brief As the int overload, into an unsigned variable.
  /// @param names    The flag's spellings.
  /// @param metavar  The value's name in the usage line.
  /// @param out      The variable it sets.
  /// @param min      The smallest value accepted.
  /// @return This parser, for chaining.
  Cli& option(Names names, const char* metavar, std::uint32_t& out,
              std::uint32_t min);

  /// @brief A flag handled by @p handler, given its value -- for a value
  ///        with a grammar of its own (`WxH`, `x,y,z`, a name from a list).
  ///        The handler's refusal is the parse's.
  /// @param names    The flag's spellings.
  /// @param metavar  The value's name in the usage line; null for a switch,
  ///                 whose handler is given a null value.
  /// @param handler  What the flag does with its value.
  /// @return This parser, for chaining.
  Cli& on(Names names, const char* metavar, Handler handler);

  /// @brief Make the flag declared as @p name one every command line must
  ///        give, or, in a @ref one_of group, the group: the usage line
  ///        shows the flag without brackets, the group in parentheses.
  /// @param name  Any spelling of the flag.
  /// @pre @p name was declared.
  /// @return This parser, for chaining.
  Cli& require(const char* name);

  /// @brief Group flags already declared, of which a command line gives one:
  ///        a second, different one is refused. The usage line shows them
  ///        where the first was declared, `[--serial SN | --rig sync.json]`,
  ///        or `(--up-axis a | --up-vector v)` once @ref require "required".
  /// @param names  Any spelling of each flag.
  /// @pre There are two names or more, and each was declared.
  /// @return This parser, for chaining.
  Cli& one_of(std::initializer_list<const char*> names);

  /// @brief Switches that each set @p out to their own value, a @ref one_of
  ///        group: `[--hevc | --mjpeg]`.
  /// @param switches  Each switch's spelling and value.
  /// @param out       The variable they set.
  /// @return This parser, for chaining.
  template <typename T>
  Cli& choice(const std::vector<std::pair<const char*, T>>& switches, T& out) {
    const int group = next_group_;
    for (const std::pair<const char*, T>& option : switches) {
      on(option.first, nullptr,
         [&out, value = option.second](const std::string&, const char*) {
           out = value;
           return Status{};
         });
      join(flags_.back().group, group);
    }
    return *this;
  }

  /// @brief A required positional argument, in declaration order.
  /// @param name  Its name in the usage line and errors.
  /// @param out   The variable it sets.
  /// @return This parser, for chaining.
  Cli& positional(const char* name, std::string& out);

  /// @brief A check run after every argument is read: a rule between flags,
  ///        or a default derived from another flag. Its refusal is the
  ///        parse's.
  /// @param rule  The check.
  /// @return This parser, for chaining.
  Cli& check(std::function<Status()> rule);

  /// @brief Read @p argv, then run the checks.
  /// @param argc  The number of entries in @p argv.
  /// @param argv  The command line; its first entry, the program, is skipped.
  /// @return OK, or InvalidArgument: `<program>: <what>`, then the usage.
  Status parse(int argc, const char* const* argv) const;

  /// @return `usage: <program> <positional>... [flag value]...`, wrapped
  ///         under the program's name before 80 columns.
  std::string usage() const;

 private:
  struct Flag {
    std::vector<std::string> names;
    std::optional<std::string> metavar;  // empty: a switch
    Handler handler;
    int group;      // a one_of group's flags share one, shown together
    bool required;  // makes its group required
  };
  struct Positional {
    std::string name;
    std::string* out;
  };

  static Status assign(const std::string& flag, const char* value,
                       std::string& out);
  static Status assign(const std::string& flag, const char* value,
                       std::optional<float>& out);
  template <typename T>
  static Status assign(const std::string& flag, const char* value, T& out) {
    return parse_number(flag, value, out);
  }

  // The index of the flag spelt @p arg; flags_.size() when none is.
  std::size_t index_of(const std::string& arg) const;
  // The flag spelt @p name, which must have been declared.
  Flag& declared(const char* name);
  // Move every flag of group @p from into group @p to.
  void join(int from, int to);
  bool group_required(int group) const;
  // "--a or --b": the flags of @p group.
  std::string names_of(int group) const;
  Status error(const std::string& what) const;

  std::string program_;
  std::vector<Flag> flags_;
  std::vector<Positional> positionals_;
  std::vector<std::function<Status()>> checks_;
  int next_group_ = 0;
};

}  // namespace volumetric_kit::core
