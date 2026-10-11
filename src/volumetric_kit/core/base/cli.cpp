// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/cli.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {

namespace {

// Whether @p text starts with white space, which strtod and strtoll would
// skip: a number is the whole text.
bool starts_with_space(const char* text) {
  return std::isspace(static_cast<unsigned char>(text[0])) != 0;
}

// The usage line's width, before which it wraps.
constexpr std::size_t kColumns = 80;

}  // namespace

Status parse_number(const std::string& flag, const char* text, double& out) {
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text, &end);
  if (starts_with_space(text) || end == text || *end != '\0' || errno != 0 ||
      !std::isfinite(value)) {
    return Status::invalid_argument(flag + ": not a finite number: " + text);
  }
  out = value;
  return {};
}

Status parse_number(const std::string& flag, const char* text, float& out) {
  double value = 0.0;
  VKC_TRY(parse_number(flag, text, value));
  if (std::abs(value) > std::numeric_limits<float>::max()) {
    return Status::invalid_argument(flag + ": number too large: " + text);
  }
  out = static_cast<float>(value);
  return {};
}

Status parse_number(const std::string& flag, const char* text, int& out) {
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(text, &end, 10);
  if (starts_with_space(text) || end == text || *end != '\0' || errno != 0 ||
      value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return Status::invalid_argument(flag + ": not an integer: " + text);
  }
  out = static_cast<int>(value);
  return {};
}

Status parse_number(const std::string& flag, const char* text,
                    std::uint32_t& out) {
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (std::isdigit(static_cast<unsigned char>(text[0])) == 0 || *end != '\0' ||
      errno != 0 || value > std::numeric_limits<std::uint32_t>::max()) {
    return Status::invalid_argument(flag + ": not a count: " + text);
  }
  out = static_cast<std::uint32_t>(value);
  return {};
}

Cli::Cli(std::string program) : program_(std::move(program)) {}

Cli& Cli::flag(Names names, bool& out, bool value) {
  return on(std::move(names), nullptr,
            [&out, value](const std::string&, const char*) {
              out = value;
              return Status{};
            });
}

Cli& Cli::option(Names names, const char* metavar, int& out, int min) {
  return on(std::move(names), metavar,
            [&out, min](const std::string& flag, const char* value) {
              int parsed = 0;
              VKC_TRY(parse_number(flag, value, parsed));
              if (parsed < min) {
                return Status::invalid_argument(
                    flag + " must be >= " + std::to_string(min));
              }
              out = parsed;
              return Status{};
            });
}

Cli& Cli::option(Names names, const char* metavar, std::uint32_t& out,
                 std::uint32_t min) {
  return on(std::move(names), metavar,
            [&out, min](const std::string& flag, const char* value) {
              std::uint32_t parsed = 0;
              VKC_TRY(parse_number(flag, value, parsed));
              if (parsed < min) {
                return Status::invalid_argument(
                    flag + " must be >= " + std::to_string(min));
              }
              out = parsed;
              return Status{};
            });
}

Cli& Cli::on(Names names, const char* metavar, Handler handler) {
  flags_.push_back({std::move(names.list), metavar, std::move(handler),
                    next_group_++, false});
  return *this;
}

Cli& Cli::require(const char* name) {
  declared(name).required = true;
  return *this;
}

Cli& Cli::one_of(std::initializer_list<const char*> names) {
  VKC_CHECK(names.size() >= 2, "Cli::one_of groups two flags or more");
  const int group = declared(*names.begin()).group;
  for (const char* name : names) join(declared(name).group, group);
  return *this;
}

Cli& Cli::positional(const char* name, std::string& out) {
  positionals_.push_back({name, &out});
  return *this;
}

Cli& Cli::check(std::function<Status()> rule) {
  checks_.push_back(std::move(rule));
  return *this;
}

Status Cli::parse(int argc, const char* const* argv) const {
  std::size_t positional = 0;
  // Per group, the flag given first: a group takes one.
  std::vector<const Flag*> given(static_cast<std::size_t>(next_group_));
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const std::size_t index = index_of(arg);
    if (index == flags_.size()) {
      if (!arg.empty() && arg[0] == '-') return error("unknown flag: " + arg);
      if (positional == positionals_.size()) {
        return error("unexpected argument: " + arg);
      }
      *positionals_[positional++].out = arg;
      continue;
    }
    const Flag& flag = flags_[index];
    const Flag*& first = given[static_cast<std::size_t>(flag.group)];
    if (first != nullptr && first != &flag) {
      return error(names_of(flag.group) + ", not both");
    }
    first = &flag;
    const char* value = nullptr;
    if (flag.metavar != nullptr) {
      if (i + 1 >= argc) return error(arg + " needs a value");
      value = argv[++i];
    }
    if (const Status s = flag.handler(arg, value); !s.ok()) {
      return error(s.message());
    }
  }
  if (positional < positionals_.size()) {
    return error(std::string("needs <") + positionals_[positional].name + ">");
  }
  for (const Flag& flag : flags_) {
    if (group_required(flag.group) &&
        given[static_cast<std::size_t>(flag.group)] == nullptr) {
      return error("needs " + names_of(flag.group));
    }
  }
  for (const std::function<Status()>& rule : checks_) {
    if (const Status s = rule(); !s.ok()) return error(s.message());
  }
  return {};
}

std::string Cli::usage() const {
  const std::string head = "usage: " + program_;
  std::vector<std::string> items;
  items.reserve(positionals_.size() + flags_.size());
  for (const Positional& p : positionals_) {
    items.push_back("<" + std::string(p.name) + ">");
  }
  // Each group once, where its first flag was declared.
  std::vector<bool> shown(static_cast<std::size_t>(next_group_), false);
  for (const Flag& flag : flags_) {
    if (shown[static_cast<std::size_t>(flag.group)]) continue;
    shown[static_cast<std::size_t>(flag.group)] = true;
    std::string item;
    bool several = false;
    for (const Flag& member : flags_) {
      if (member.group != flag.group) continue;
      if (!item.empty()) {
        item += " | ";
        several = true;
      }
      item += member.names.front();
      if (member.metavar != nullptr) {
        item += ' ';
        item += member.metavar;
      }
    }
    if (!group_required(flag.group)) {
      item.insert(0, 1, '[').push_back(']');
    } else if (several) {
      item.insert(0, 1, '(').push_back(')');
    }
    items.push_back(std::move(item));
  }
  const std::string indent(head.size() + 1, ' ');
  std::string out = head;
  std::size_t line = out.size();
  for (const std::string& item : items) {
    if (line + 1 + item.size() >= kColumns && line > indent.size()) {
      out += '\n';
      out += indent;
      line = indent.size();
    } else {
      out += ' ';
      line += 1;
    }
    out += item;
    line += item.size();
  }
  return out;
}

Status Cli::assign(const std::string& /*flag*/, const char* value,
                   std::string& out) {
  out = value;
  return {};
}

Status Cli::assign(const std::string& flag, const char* value,
                   std::optional<float>& out) {
  float parsed = 0.0F;
  VKC_TRY(parse_number(flag, value, parsed));
  out = parsed;
  return {};
}

std::size_t Cli::index_of(const std::string& arg) const {
  for (std::size_t f = 0; f < flags_.size(); ++f) {
    const std::vector<std::string>& names = flags_[f].names;
    if (std::find(names.begin(), names.end(), arg) != names.end()) return f;
  }
  return flags_.size();
}

Cli::Flag& Cli::declared(const char* name) {
  const std::size_t index = index_of(name);
  VKC_CHECK(index < flags_.size(), "Cli: a flag named before it was declared");
  return flags_[index];
}

void Cli::join(int from, int to) {
  for (Flag& flag : flags_) {
    if (flag.group == from) flag.group = to;
  }
}

bool Cli::group_required(int group) const {
  return std::any_of(flags_.begin(), flags_.end(), [group](const Flag& flag) {
    return flag.group == group && flag.required;
  });
}

std::string Cli::names_of(int group) const {
  std::string names;
  for (const Flag& flag : flags_) {
    if (flag.group != group) continue;
    names += (names.empty() ? "" : " or ") + flag.names.front();
  }
  return names;
}

Status Cli::error(const std::string& what) const {
  return Status::invalid_argument(program_ + ": " + what + "\n" + usage());
}

}  // namespace volumetric_kit::core
