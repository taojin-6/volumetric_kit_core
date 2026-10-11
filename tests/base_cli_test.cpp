// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Cli and parse_number, moved from recon's example_cli_test: numbers read
// whole and finite, a flag's value taken whatever it looks like, unknown and
// missing arguments refused with the usage line, required flags and one-of
// groups, checks run after the arguments, and the usage line's layout. Pure
// host, so they run everywhere.

#include "volumetric_kit/core/base/cli.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core {
namespace {

// Parse `args` (the program name is prepended) with `cli`.
Status parse(const Cli& cli, const std::vector<const char*>& args) {
  std::vector<const char*> argv{"prog"};
  argv.insert(argv.end(), args.begin(), args.end());
  return cli.parse(static_cast<int>(argv.size()), argv.data());
}

bool has(const Status& s, const std::string& text) {
  return s.message().find(text) != std::string::npos;
}

TEST(CliNumbers, RealsAreTheWholeTextAndFinite) {
  double d = 7.0;
  EXPECT_TRUE(parse_number("--x", "0.25", d).ok());
  EXPECT_EQ(d, 0.25);
  EXPECT_TRUE(parse_number("--x", "-1e3", d).ok());
  EXPECT_EQ(d, -1000.0);
  // The whole text, finite, in range; a refusal leaves the value alone.
  for (const char* bad :
       {"", "10x", "0.5 ", " 0.5", "nan", "inf", "1e999", "x"}) {
    d = 7.0;
    const Status s = parse_number("--x", bad, d);
    EXPECT_EQ(s.domain(), Status::Code::InvalidArgument) << bad;
    EXPECT_TRUE(has(s, "--x: not a finite number")) << bad;
    EXPECT_EQ(d, 7.0) << bad;
  }
  float f = 0.0F;
  for (const char* large : {"1e39", "-1e39"}) {
    EXPECT_TRUE(has(parse_number("--x", large, f), "--x: number too large"))
        << large;
    EXPECT_EQ(f, 0.0F) << large;
  }
  EXPECT_TRUE(parse_number("--x", "-2.5", f).ok());
  EXPECT_EQ(f, -2.5F);
}

// A value too small for its type rounds toward zero, in either type.
TEST(CliNumbers, AnUnderflowRoundsTowardZero) {
  double d = 7.0;
  ASSERT_TRUE(parse_number("--x", "1e-310", d).ok());
  EXPECT_GT(d, 0.0);
  EXPECT_LT(d, std::numeric_limits<double>::min());
  ASSERT_TRUE(parse_number("--x", "-1e-400", d).ok());
  EXPECT_EQ(d, 0.0);
  float f = 7.0F;
  ASSERT_TRUE(parse_number("--x", "1e-50", f).ok());
  EXPECT_EQ(f, 0.0F);
}

TEST(CliNumbers, IntegersAreDecimalAndInRange) {
  int i = 3;
  EXPECT_TRUE(parse_number("--n", "-42", i).ok());
  EXPECT_EQ(i, -42);
  EXPECT_TRUE(parse_number("--n", "-2147483648", i).ok());
  EXPECT_EQ(i, std::numeric_limits<int>::min());
  EXPECT_TRUE(parse_number("--n", "2147483647", i).ok());
  EXPECT_EQ(i, std::numeric_limits<int>::max());
  for (const char* bad :
       {"10x", "", " 5", "1.5", "2147483648", "-2147483649", "0x10"}) {
    i = 3;
    EXPECT_TRUE(has(parse_number("--n", bad, i), "--n: not an integer")) << bad;
    EXPECT_EQ(i, 3) << bad;
  }
}

TEST(CliNumbers, ACountIsDigitsOnly) {
  std::uint32_t u = 9;
  EXPECT_TRUE(parse_number("--n", "4294967295", u).ok());
  EXPECT_EQ(u, 4294967295U);
  // No sign, no white space, nothing past uint32_t.
  for (const char* bad : {"-1", "-0", "+1", " 1", "4294967296", "1e3", ""}) {
    EXPECT_TRUE(has(parse_number("--n", bad, u), "--n: not a count")) << bad;
    EXPECT_EQ(u, 4294967295U) << bad;
  }
}

struct Options {
  std::string scene, out = "a.ply", up;
  float voxel = 0.02F;
  double scale = 1.0;
  int frames = 5;
  std::uint32_t fps = 30;
  bool preload = false, texture = true, lit = false;
  std::optional<float> depth;
};

// The declarations recon's examples make, of every kind.
class CliParse : public ::testing::Test {
 protected:
  CliParse() {
    cli_.positional("scene_dir", o_.scene)
        .option({"-o", "--out"}, "out.ply", o_.out)
        .option("--voxel", "m", o_.voxel)
        .option("--scale", "s", o_.scale)
        .option("--frames", "N", o_.frames, 1)
        .option("--fps", "N", o_.fps, 1U)
        .option("--depth", "m", o_.depth)
        .option("--up", "axis", o_.up)
        .flag("--preload", o_.preload)
        .flag("--no-texture", o_.texture, false)
        .choice<bool>({{"--lit", true}, {"--unlit", false}}, o_.lit)
        .check([this] {
          ++checks_;
          return Status{};
        });
  }

  // Whether `args` is refused with `why` after the program's name, then the
  // usage line.
  bool refused(const std::vector<const char*>& args, const char* why) const {
    const Status s = parse(cli_, args);
    return s.domain() == Status::Code::InvalidArgument &&
           s.message().rfind(std::string("prog: ") + why, 0) == 0 &&
           has(s, "\nusage: prog <scene_dir>");
  }

  Options o_;
  int checks_ = 0;
  Cli cli_{"prog"};
};

// Every kind of declaration sets its variable; an alias is the flag; a value
// is taken however it starts; a flag given twice keeps the last.
TEST_F(CliParse, EveryDeclarationSetsItsVariable) {
  ASSERT_TRUE(
      parse(cli_, {"room0", "--out", "b.ply", "--voxel", "0.01", "--scale",
                   "2.5", "--frames", "7", "--fps", "25", "--depth", "3",
                   "--up", "-y", "--preload", "--no-texture", "--lit", "--lit"})
          .ok());
  EXPECT_EQ(o_.scene, "room0");
  EXPECT_EQ(o_.out, "b.ply");
  EXPECT_EQ(o_.voxel, 0.01F);
  EXPECT_EQ(o_.scale, 2.5);
  EXPECT_EQ(o_.frames, 7);
  EXPECT_EQ(o_.fps, 25U);
  EXPECT_EQ(o_.depth, 3.0F);
  EXPECT_EQ(o_.up, "-y");
  EXPECT_TRUE(o_.preload);
  EXPECT_FALSE(o_.texture);
  EXPECT_TRUE(o_.lit);
  EXPECT_EQ(checks_, 1);

  EXPECT_TRUE(
      parse(cli_, {"-o", "c.ply", "room0", "--frames", "2", "--frames", "3"})
          .ok());
  EXPECT_EQ(o_.out, "c.ply");
  EXPECT_EQ(o_.frames, 3);
  // Each parse stands alone: the earlier --lit does not refuse --unlit.
  EXPECT_TRUE(parse(cli_, {"room0", "--unlit"}).ok());
  EXPECT_FALSE(o_.lit);
  // A bound is accepted itself.
  ASSERT_TRUE(parse(cli_, {"room0", "--frames", "1", "--fps", "1"}).ok());
  EXPECT_EQ(o_.frames, 1);
  EXPECT_EQ(o_.fps, 1U);
}

// Each refusal names its program and its cause, then the usage line.
TEST_F(CliParse, RefusalsNameTheCauseThenTheUsage) {
  EXPECT_TRUE(refused({}, "needs <scene_dir>"));
  EXPECT_TRUE(refused({"a", "b"}, "unexpected argument: b"));
  EXPECT_TRUE(refused({"a", "--bogus"}, "unknown flag: --bogus"));
  EXPECT_TRUE(refused({"a", "-"}, "unknown flag: -"));
  EXPECT_TRUE(refused({"a", "--voxel"}, "--voxel needs a value"));
  EXPECT_TRUE(
      refused({"a", "--frames", "10x"}, "--frames: not an integer: 10x"));
  EXPECT_TRUE(refused({"a", "--frames", "0"}, "--frames must be >= 1"));
  EXPECT_TRUE(refused({"a", "--fps", "0"}, "--fps must be >= 1"));
  EXPECT_TRUE(refused({"a", "--fps", "-1"}, "--fps: not a count: -1"));
  EXPECT_TRUE(refused({"a", "--voxel", "nan"}, "--voxel: not a finite number"));
  EXPECT_TRUE(refused({"a", "--depth", "x"}, "--depth: not a finite number"));
  EXPECT_TRUE(refused({"a", "--lit", "--unlit"}, "--lit or --unlit, not both"));
  // A refused bound leaves the variable as it was.
  EXPECT_EQ(o_.frames, 5);
  EXPECT_EQ(o_.fps, 30U);
}

// A check runs after every argument, and its refusal is the parse's.
TEST(Cli, ChecksRunAfterTheArguments) {
  int frames = 0;
  int seen = 0;
  Cli checked("prog");
  checked.option("--frames", "N", frames).check([&] {
    seen = frames;
    return Status::invalid_argument("frames is " + std::to_string(frames));
  });
  const Status late = parse(checked, {"--frames", "3"});
  EXPECT_EQ(seen, 3);
  EXPECT_TRUE(has(late, "prog: frames is 3\nusage: prog"));
}

// Checks run in declaration order, and the first refusal is the parse's: the
// checks after it do not run.
TEST(Cli, ChecksRunInOrderUntilOneRefuses) {
  std::vector<int> ran;
  bool refuse = false;
  Cli cli("prog");
  cli.check([&] {
       ran.push_back(1);
       return refuse ? Status::invalid_argument("first") : Status{};
     })
      .check([&] {
        ran.push_back(2);
        return refuse ? Status::invalid_argument("second") : Status{};
      });
  ASSERT_TRUE(parse(cli, {}).ok());
  EXPECT_EQ(ran, (std::vector<int>{1, 2}));
  ran.clear();
  refuse = true;
  EXPECT_TRUE(has(parse(cli, {}), "prog: first\nusage: prog"));
  EXPECT_EQ(ran, std::vector<int>{1});
}

// A handler gets the spelling it was found under and the value, or null for
// a switch; its refusal is the parse's.
TEST(Cli, AHandlerSeesItsSpellingAndValue) {
  std::string spelt;
  const char* got = "unset";
  Cli cli("prog");
  cli.on({"-s", "--size"}, "WxH",
         [&](const std::string& flag, const char* value) {
           spelt = flag;
           got = value;
           if (std::string(value).find('x') == std::string::npos) {
             return Status::invalid_argument(flag + " needs WxH");
           }
           return Status{};
         })
      .on("--now", nullptr, [&](const std::string& flag, const char* value) {
        spelt = flag;
        got = value;
        return Status{};
      });
  ASSERT_TRUE(parse(cli, {"-s", "4x3"}).ok());
  EXPECT_EQ(spelt, "-s");
  EXPECT_STREQ(got, "4x3");
  ASSERT_TRUE(parse(cli, {"--now"}).ok());
  EXPECT_EQ(spelt, "--now");
  EXPECT_EQ(got, nullptr);
  EXPECT_TRUE(
      has(parse(cli, {"--size", "4"}), "prog: --size needs WxH\nusage:"));
}

// Positional arguments fill in declaration order.
TEST(Cli, PositionalsFillInOrder) {
  std::string in;
  std::string out;
  Cli cli("prog");
  cli.positional("in", in).positional("out", out);
  EXPECT_TRUE(has(parse(cli, {"a"}), "prog: needs <out>"));
  ASSERT_TRUE(parse(cli, {"a", "b"}).ok());
  EXPECT_EQ(in, "a");
  EXPECT_EQ(out, "b");
}

// A required flag is refused when absent and shown without brackets.
TEST(Cli, ARequiredFlagMustBeGiven) {
  std::string rig;
  Cli required("prog");
  required.option("--rig", "sync.json", rig).require("--rig");
  EXPECT_TRUE(has(parse(required, {}), "prog: needs --rig"));
  ASSERT_TRUE(parse(required, {"--rig", "r.json"}).ok());
  EXPECT_EQ(rig, "r.json");
  EXPECT_EQ(required.usage(), "usage: prog --rig sync.json");
}

// A one_of group of flags that take values, declared apart: one of them, the
// last when repeated; a second, different one is refused. Required, the
// group needs one of its flags.
TEST(Cli, AOneOfGroupTakesOneFlag) {
  std::string serial;
  std::string rig;
  int frames = 0;
  Cli grouped("prog");
  grouped.option("--serial", "SN", serial)
      .option("--frames", "N", frames)
      .option("--rig", "sync.json", rig)
      .one_of({"--serial", "--rig"});
  EXPECT_TRUE(parse(grouped, {}).ok());
  ASSERT_TRUE(parse(grouped, {"--rig", "a.json", "--rig", "b.json"}).ok());
  EXPECT_EQ(rig, "b.json");
  EXPECT_TRUE(has(parse(grouped, {"--serial", "1", "--rig", "r.json"}),
                  "prog: --serial or --rig, not both"));
  EXPECT_TRUE(
      has(parse(grouped, {"--rig", "r.json", "--frames", "2", "--serial", "1"}),
          "prog: --serial or --rig, not both"));
  grouped.require("--rig");
  EXPECT_TRUE(
      has(parse(grouped, {"--frames", "2"}), "prog: needs --serial or --rig"));
  ASSERT_TRUE(parse(grouped, {"--serial", "7"}).ok());
  EXPECT_EQ(serial, "7");
}

TEST(CliUsage, WrapsUnderTheProgramBefore80Columns) {
  std::string s;
  bool b = false;
  int n = 0;
  Cli cli("tool");
  cli.positional("in", s)
      .option({"-o", "--out"}, "path", s)
      .flag("--fast", b)
      .choice<int>({{"--one", 1}, {"--two", 2}}, n)
      .option("--a-rather-long-flag-name", "value", s)
      .option("--another-long-flag", "value", s)
      .option("--third", "value", s);
  EXPECT_EQ(cli.usage(),
            "usage: tool <in> [-o path] [--fast] [--one | --two]\n"
            "            [--a-rather-long-flag-name value] "
            "[--another-long-flag value]\n"
            "            [--third value]");
}

// `tool`'s usage line with one optional flag, its item `[--aa...a v]`
// @p width columns wide, after a positional `<in>` when @p positional.
std::string usage_with_item(bool positional, std::size_t width) {
  std::string s;
  const std::string name = "--" + std::string(width - 6, 'a');
  Cli cli("tool");
  if (positional) cli.positional("in", s);
  cli.option(name.c_str(), "v", s);
  return cli.usage();
}

// No line reaches column 80: an item that would end there starts the next
// line, under the program's name. The first item stays beside the name
// however wide it is.
TEST(CliUsage, NoLineReachesColumn80) {
  // `usage: tool <in>` is 16 columns, then a space and the item.
  const std::string fits = usage_with_item(true, 62);
  EXPECT_EQ(fits.size(), 79U);
  EXPECT_EQ(fits.find('\n'), std::string::npos);
  EXPECT_EQ(usage_with_item(true, 63).rfind(
                "usage: tool <in>\n" + std::string(12, ' ') + "[--a", 0),
            0U);
  const std::string wide = usage_with_item(false, 90);
  EXPECT_EQ(wide.rfind("usage: tool [--a", 0), 0U);
  EXPECT_EQ(wide.find('\n'), std::string::npos);
}

// A metavar and a positional's name are copied, so they may be temporaries.
TEST(CliUsage, NamesAreCopied) {
  std::string s;
  Cli cli("tool");
  {
    std::string name = "in";
    std::string metavar = "path";
    cli.positional(name.c_str(), s).option("--out", metavar.c_str(), s);
    name.assign("XX");  // in place: a borrowed pointer would now read these
    metavar.assign("XXXX");
  }
  EXPECT_EQ(cli.usage(), "usage: tool <in> [--out path]");
  EXPECT_TRUE(has(parse(cli, {}), "tool: needs <in>"));
}

// A group is shown where its first flag was declared: in brackets when
// optional, in parentheses when required.
TEST(CliUsage, AGroupShowsWhereItsFirstFlagWasDeclared) {
  std::string s;
  int n = 0;
  Cli grouped("tool");
  grouped.option("--serial", "SN", s)
      .option("--frames", "N", n)
      .option("--rig", "sync.json", s)
      .one_of({"--serial", "--rig"});
  EXPECT_EQ(grouped.usage(),
            "usage: tool [--serial SN | --rig sync.json] [--frames N]");
  grouped.require("--serial");
  EXPECT_EQ(grouped.usage(),
            "usage: tool (--serial SN | --rig sync.json) [--frames N]");
}

// Naming a flag that was never declared, grouping fewer than two, or
// declaring a spelling twice is a programmer error.
TEST(CliDeathTest, MisdeclarationsAbort) {
  std::string s;
  bool b = false;
  EXPECT_DEATH(
      {
        Cli cli("prog");
        cli.require("--rig");
      },
      "Cli: a flag named before it was declared");
  EXPECT_DEATH(
      {
        Cli cli("prog");
        cli.option("--serial", "SN", s).one_of({"--serial", "--rig"});
      },
      "Cli: a flag named before it was declared");
  EXPECT_DEATH(
      {
        Cli cli("prog");
        cli.option("--serial", "SN", s).one_of({"--serial"});
      },
      "Cli::one_of groups two flags or more");
  EXPECT_DEATH(
      {
        Cli cli("prog");
        cli.option({"-o", "--out"}, "path", s).flag("--out", b);
      },
      "Cli: a spelling declared twice");
  EXPECT_DEATH(
      {
        Cli cli("prog");
        cli.flag("--lit", b)
            .choice<bool>({{"--unlit", false}, {"--lit", true}}, b);
      },
      "Cli: a spelling declared twice");
}

}  // namespace
}  // namespace volumetric_kit::core
