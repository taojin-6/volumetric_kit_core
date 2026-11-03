// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The net tier's address checks as an address field shows them
// (net/address.hpp), each refusal by its whole message: a port is 1 to 65535
// in decimal digits alone, with no sign, space or leading zero; "host" takes
// the caller's default port, or names none when that is 0, "host:port" its
// own, and the host is checked no further; IPv6, by a leading '[' or a second
// ':', each by its own reason, is refused before the split at the colon; and
// one host's numeric address is never empty, a name, a part over 255 or with
// a leading zero (on Apple too, whose inet_pton takes one), or the wildcard.
// resolve takes a number in dotted decimal alone, never the wildcard: a
// short, octal or hex form, which getaddrinfo reads as glibc's or Apple's
// rules have it, digits and dots that are no address, or IPv6, which glibc's
// getaddrinfo reads as IPv4 when mapped, are refused before any lookup; a
// name, numeric labels and all, is still looked up.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/net/address.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace vkc = volumetric_kit::core;
namespace net = vkc::net;

namespace {

using Code = vkc::Status::Code;

// The InvalidArgument message of a refusal; empty for anything else.
template <typename T>
std::string refusal(const vkc::Result<T>& result) {
  if (result.ok() || result.status().domain() != Code::InvalidArgument) {
    return {};
  }
  return result.status().message();
}

std::string not_a_port(std::string_view text) {
  return "\"" + std::string(text) +
         "\" is not a port: a number from 1 to 65535, with no leading zero";
}

std::string quoted(std::string_view text, std::string_view reason) {
  return "\"" + std::string(text) + "\"" + std::string(reason);
}

int port_case() {
  for (const auto& [text, port] :
       {std::pair{"1", 1U}, std::pair{"9", 9U}, std::pair{"10", 10U},
        std::pair{"7101", 7101U}, std::pair{"47801", 47801U},
        std::pair{"65535", 65535U}}) {
    const vkc::Result<std::uint16_t> parsed = net::parse_port(text);
    CHECK(parsed.ok() && *parsed == port);
  }
  // 4294974397 and 4294967297 wrap a 32-bit sum to 7101 and 1, so only the
  // five-digit limit refuses them.
  for (const char* text :
       {"0", "65536", "99999", "100000", "99999999999", "4294974397",
        "4294967297", "", "+1", "-1", " 1", "1 ", "007", "07101", "00", "1x",
        "0x1F", "1.5", "1e3"}) {
    CHECK(refusal(net::parse_port(text)) == not_a_port(text));
  }
  return 0;
}

int host_port_case() {
  // Its own port, or the default; a name or a numeric address alike.
  for (const auto& [text, default_port, host, port] :
       {std::tuple{"lab-box:7101", 47801, "lab-box", 7101},
        std::tuple{"lab-box:7101", 0, "lab-box", 7101},
        std::tuple{"lab-box", 47801, "lab-box", 47801},
        std::tuple{"10.8.0.1:1", 0, "10.8.0.1", 1},
        std::tuple{"10.8.0.1:65535", 0, "10.8.0.1", 65535},
        std::tuple{"256.1.1.1:80", 0, "256.1.1.1", 80},
        std::tuple{"0.0.0.0", 47801, "0.0.0.0", 47801}}) {
    const vkc::Result<net::HostPort> parsed =
        net::parse_host_port(text, static_cast<std::uint16_t>(default_port));
    CHECK(parsed.ok() && parsed->host == host && parsed->port == port);
  }
  // No host, no port, a bad port: the port's own reason quotes the port.
  for (const auto& [text, default_port, reason] :
       {std::tuple{"", 47801, quoted("", " names no host")},
        std::tuple{"", 0, quoted("", " names no host")},
        std::tuple{":7101", 0, quoted(":7101", " names no host")},
        std::tuple{":", 47801, quoted(":", " names no host")},
        std::tuple{"lab-box", 0, quoted("lab-box", " names no port")},
        std::tuple{"lab-box:", 47801, not_a_port("")},
        std::tuple{"lab-box:0", 47801, not_a_port("0")},
        std::tuple{"lab-box:65536", 0, not_a_port("65536")},
        std::tuple{"lab-box:007", 0, not_a_port("007")},
        std::tuple{"lab-box: 7101", 0, not_a_port(" 7101")},
        std::tuple{"lab-box:7101/x", 0, not_a_port("7101/x")}}) {
    CHECK(refusal(net::parse_host_port(
              text, static_cast<std::uint16_t>(default_port))) == reason);
  }
  // IPv6, before the split at the colon would call it a bad port or no host:
  // a leading '[', or a second ':', named as such since a pasted
  // "tcp://host:port" or a port with more after it has one too.
  for (const char* text : {"[::1]:1", "[::1]", "[", "[10.8.0.1]:7101"}) {
    for (const std::uint16_t default_port : {0, 47801}) {
      CHECK(refusal(net::parse_host_port(text, default_port)) ==
            quoted(text, " is IPv6, which is not supported"));
    }
  }
  for (const char* text : {"::1", "::", "a:1:2", "fe80::1",
                           "lab-box:1:", "10.8.0.1:7101:", "10.8.0.1:7101:7102",
                           "tcp://10.8.0.1:7101"}) {
    for (const std::uint16_t default_port : {0, 47801}) {
      CHECK(refusal(net::parse_host_port(text, default_port)) ==
            quoted(text, " has a second ':'; IPv6 is not supported"));
    }
  }
  return 0;
}

int endpoint_case() {
  for (const auto& [host, port, shown] :
       {std::tuple{"10.8.0.1", 7101, "10.8.0.1:7101"},
        std::tuple{"0.0.0.1", 1, "0.0.0.1:1"},
        std::tuple{"255.255.255.255", 65535, "255.255.255.255:65535"},
        std::tuple{"127.0.0.1", 0, "127.0.0.1:0"}}) {
    const vkc::Result<net::Endpoint> parsed =
        net::parse_endpoint(host, static_cast<std::uint16_t>(port));
    CHECK(parsed.ok() && net::to_string(*parsed) == shown);
  }
  CHECK(refusal(net::parse_endpoint("", 7101)) == "the host is empty");
  CHECK(refusal(net::parse_endpoint("0.0.0.0", 7101)) ==
        "0.0.0.0 is the wildcard, not one host's address");
  for (const char* host :
       {"lab-box", "localhost", "::1", "[::1]", "10.8.0", "10.8.0.1.2",
        "10..0.1", "10.8.0.256", "010.8.0.1", "10.8.0.1 ", " 10.8.0.1"}) {
    CHECK(refusal(net::parse_endpoint(host, 7101)) ==
          quoted(host, " is not a numeric IPv4 address"));
  }
  return 0;
}

// A leading zero in any part is no address on every platform, though Apple's
// inet_pton takes one; a part that is 0 alone is one.
int leading_zero_case() {
  for (const char* host :
       {"010.8.0.1", "10.08.0.1", "10.8.00.1", "10.8.0.01", "0010.1.1.1",
        "1.2.3.0255", "00.0.0.1", "0.0.0.00", "000.0.0.0"}) {
    CHECK(!net::parse_ipv4(host, 1).has_value());
  }
  for (const auto& [host, ipv4] :
       {std::pair{"0.0.0.0", 0x00000000U}, std::pair{"10.0.0.1", 0x0A000001U},
        std::pair{"10.8.0.10", 0x0A08000AU}, std::pair{"0.10.0.0", 0x000A0000U},
        std::pair{"100.200.0.0", 0x64C80000U}}) {
    const std::optional<net::Endpoint> parsed = net::parse_ipv4(host, 1);
    CHECK(parsed.has_value() && parsed->ipv4 == ipv4);
  }
  return 0;
}

// The endpoints resolve gives, as "a.b.c.d:port"; empty on an error.
std::vector<std::string> resolved(const std::string& host) {
  const vkc::Result<std::vector<net::Endpoint>> found =
      net::resolve("test", host, 7101);
  std::vector<std::string> shown;
  if (found.ok()) {
    for (const net::Endpoint& endpoint : *found) {
      shown.push_back(net::to_string(endpoint));
    }
  }
  return shown;
}

int resolve_case() {
  // Dotted decimal reads as itself, up to a NUL as getaddrinfo reads it; a
  // name is looked up.
  CHECK(resolved("10.8.0.1") == std::vector<std::string>{"10.8.0.1:7101"});
  CHECK(resolved("255.255.255.255") ==
        std::vector<std::string>{"255.255.255.255:7101"});
  CHECK(resolved(std::string("10.8.0.1\0x", 10)) ==
        std::vector<std::string>{"10.8.0.1:7101"});
  const std::vector<std::string> localhost = resolved("localhost");
  CHECK(!localhost.empty());
  for (const std::string& endpoint : localhost) {
    CHECK(endpoint == "127.0.0.1:7101");
  }
  // Each form both systems' getaddrinfo reads as a number (glibc "010.8.0.1"
  // as 8.8.0.1, Apple as 10.8.0.1; both "10.1" as 10.0.0.1, "0x7f000001" as
  // 127.0.0.1, "0" as 0.0.0.0), and digits and dots that are no address.
  const std::string not_numeric =
      "test: the host is not a numeric IPv4 address: four decimal parts from "
      "0 to 255, with no leading zero";
  for (const char* host :
       {"010.8.0.1",  "10.08.0.1",  "10.8.0.01",  "00.0.0.0",   "10.1",
        "127.1",      "10.8.1",     "2130706433", "0",          "0.0",
        "08",         "0x7f.0.0.1", "0X7F.0.0.1", "0x7f000001", "0xdeadbeef",
        "10.0x8.0.1", "256.1.1.1",  "10.8.0.256", "1.2.3.4.5",  "10.8.0.1.",
        ".10.8.0.1",  "10..0.1",    "."}) {
    CHECK(refusal(net::resolve("test", host, 7101)) == not_numeric);
  }
  // Any ':', which no name has: glibc's getaddrinfo reads an IPv4-mapped
  // address as IPv4 ("::ffff:0.0.0.0" as 0.0.0.0, with a scope too;
  // "::ffff:7f00:1" as 127.0.0.1), where Apple's finds none.
  for (const char* host :
       {"::ffff:0.0.0.0", "::ffff:0.0.0.0%1", "::FFFF:0:0", "::ffff:7f00:1",
        "::ffff:10.8.0.1", "::1", "10.8.0.1:7101", ":"}) {
    CHECK(refusal(net::resolve("test", host, 7101)) == not_numeric);
  }
  CHECK(refusal(net::resolve("test", "0.0.0.0", 7101)) ==
        "test: 0.0.0.0 is the wildcard, not one host's address");
  for (const std::string& host : {std::string(), std::string("\0", 1)}) {
    CHECK(refusal(net::resolve("test", host, 7101)) ==
          "test: the host is empty");
  }
  // A name with numeric labels is a name, looked up; .invalid never resolves
  // (RFC 6761), so the lookup fails, with a network or without one.
  for (const char* host : {"10.8.0.1.invalid", "0x7f.invalid", "1e3.invalid"}) {
    const vkc::Result<std::vector<net::Endpoint>> found =
        net::resolve("test", host, 7101);
    CHECK(!found.ok() && found.status().domain() != Code::InvalidArgument);
    CHECK(found.status().message().rfind("test: cannot look up the name: ",
                                         0) == 0);
  }
  return 0;
}

}  // namespace

// The messages are strings, whose allocation may throw; a test that throws
// fails, as it should.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  if (port_case() != 0) return 1;
  if (host_port_case() != 0) return 1;
  if (endpoint_case() != 0) return 1;
  if (leading_zero_case() != 0) return 1;
  if (resolve_case() != 0) return 1;
  return 0;
}
