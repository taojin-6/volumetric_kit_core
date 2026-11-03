// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// core_test_base's loopback sockets (net_loopback.hpp): bind_loopback listens
// on 127.0.0.1 at a free port, which local_port names; connect_loopback
// reaches it, with the receive buffer asked for, and accept_within takes the
// connection, or none within its wait; send_bytes' bytes reach read_upto,
// which stops at the count asked for and reads the peer's end; and a
// closed_port refuses a connect.

#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "volumetric_kit/core/base/unique_fd.hpp"
#include "volumetric_kit/core/testing/net_loopback.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace vkc = volumetric_kit::core;
namespace test = volumetric_kit::core::test;

namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

int run() {
  const sockaddr_in at = test::loopback(7101);
  CHECK(at.sin_family == AF_INET && ntohs(at.sin_port) == 7101 &&
        ntohl(at.sin_addr.s_addr) == INADDR_LOOPBACK);

  const test::LoopbackListener listener = test::bind_loopback();
  CHECK(listener.fd.valid() && listener.port != 0);
  CHECK(test::local_port(listener.fd.get()) == listener.port);
  CHECK(!test::accept_within(listener.fd.get(), 10ms).valid());

  const vkc::UniqueFd reader = test::connect_loopback(listener.port, 1 << 16);
  CHECK(reader.valid());
  int buffer = 0;
  socklen_t size = sizeof buffer;
  CHECK(::getsockopt(reader.get(), SOL_SOCKET, SO_RCVBUF, &buffer, &size) ==
            0 &&
        buffer >= 1 << 16);
  vkc::UniqueFd sender = test::accept_within(listener.fd.get(), 2s);
  CHECK(sender.valid());

  CHECK(test::send_bytes(sender.get(), {1, 2, 3, 4, 5}));
  Bytes got;
  bool eof = true;
  test::read_upto(reader.get(), 3, 2s, &got, &eof);
  CHECK(got == (Bytes{1, 2, 3}) && !eof);
  sender.reset();
  test::read_upto(reader.get(), 10, 2s, &got, &eof);
  CHECK(got == (Bytes{1, 2, 3, 4, 5}) && eof);

  const std::uint16_t closed = test::closed_port();
  CHECK(closed != 0);
  CHECK(!test::connect_loopback(closed).valid());
  return 0;
}

}  // namespace

// libc++ throws from std::vector's allocation inline, where clang-tidy sees
// it; a test that throws fails, as it should.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() { return run(); }
