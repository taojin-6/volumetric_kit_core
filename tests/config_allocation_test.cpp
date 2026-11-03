// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A live source.mobile whose net checks (net::parse_host_port, then
// net::parse_endpoint) run out of memory: config::parse gives OutOfMemory,
// never the key's refusal with net's "out of memory" as its reason. The
// failed allocation is net's largest, measured from the two calls as the
// config makes them; a long host name makes it the largest of the whole
// parse, which the test checks, so no earlier allocation takes the failure.

#include <cstddef>
#include <string>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/core/net/address.hpp"
#include "volumetric_kit/core/testing/test_allocation_failure.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace vkc = volumetric_kit::core;
namespace config = vkc::config;
namespace net = vkc::net;
namespace test = vkc::test;

namespace {

using Code = vkc::Status::Code;

vkc::Result<config::Config> parse(const std::string& text) {
  return config::parse(text, config::Application::FuseOrbbec, "/scenario");
}

int out_of_memory_stays_out_of_memory() {
  // A name, which parse_endpoint refuses: both checks build a message about
  // it, and their strings grow to twice its length.
  const std::string host(3000, 'a');
  const std::string address = host + ":7101";
  const std::string text =
      R"({"version":1,"operation":"fuse","source":{"kind":"live","calibration":"rig.json","mobile":"tcp://)" +
      address + R"(","mobile_registration":"reg.json"}})";

  const vkc::Result<config::Config> refused = parse(text);
  CHECK(!refused.ok());
  CHECK(refused.status().domain() == Code::InvalidArgument);
  CHECK(refused.status().message().find("source.mobile: \"" + host +
                                        "\" is not a numeric IPv4 address") !=
        std::string::npos);

  // Net's largest allocation, as the config calls it.
  auto& failure = test::allocation_failure;
  failure = {};
  failure.measure = true;
  {
    const vkc::Result<net::HostPort> at = net::parse_host_port(address, 0);
    if (at.ok()) (void)net::parse_endpoint(at->host, at->port);
  }
  failure.measure = false;
  const std::size_t bytes = failure.largest;
  const void* site = failure.largest_site;
  CHECK(site != nullptr);
  // It is the parse's largest too.
  failure = {};
  failure.measure = true;
  (void)parse(text);
  failure.measure = false;
  const bool largest = failure.largest == bytes && failure.largest_site == site;
  failure = {};
  CHECK(largest);

  failure.bytes = bytes;
  failure.site = site;
  failure.armed = true;
  const vkc::Result<config::Config> failed = parse(text);
  const bool injected = failure.injected;
  failure = {};
  CHECK(injected);
  CHECK(!failed.ok());
  CHECK(failed.status().domain() == Code::OutOfMemory);
  return 0;
}

}  // namespace

// The scenario and the messages are strings, whose allocation may throw; a
// test that throws fails, as it should.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() { return out_of_memory_stays_out_of_memory(); }
