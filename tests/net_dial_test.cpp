// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A sender that dials out (net/dial.hpp), moved from grid's networking dial
// test with its Listener and dial cases, on loopback, 127.0.0.1 port 0 only:
// Listener refuses an empty, wildcard or named host, an address in use and
// one not this host's, takes a free port for 0, gives an empty fd with
// nothing waiting, within its wait, or for a connection reset as it waited,
// and accepted sockets the 2 MiB receive buffer, above the default, refuses
// connections once destroyed, and an accept out of descriptors is an IoError
// naming it, after which it goes on; dial refuses each bad entry with a
// message an address field can show, a closed port with the system's words
// and a connect that never finishes at its limit, and connects to a Listener
// with a non-blocking, close-on-exec socket.

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef __linux__
#include <algorithm>
#include <fstream>
#endif

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"
#include "volumetric_kit/core/net/dial.hpp"
#include "volumetric_kit/core/net/socket.hpp"
#include "volumetric_kit/core/testing/net_loopback.hpp"
#include "volumetric_kit/core/testing/test_check.hpp"

namespace vkc = volumetric_kit::core;
namespace net = vkc::net;
namespace test = vkc::test;

namespace {

using Code = vkc::Status::Code;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int option(int fd, int level, int name) {
  int value = -1;
  socklen_t size = sizeof value;
  if (::getsockopt(fd, level, name, &value, &size) != 0) return -1;
  return value;
}

bool nonblocking(int fd) { return (::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0; }
bool cloexec(int fd) { return (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0; }

std::uint16_t peer_port(int fd) {
  sockaddr_in a{};
  socklen_t size = sizeof a;
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&a), &size) != 0) return 0;
  return ntohs(a.sin_port);
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

// Whether `s` is `code` with `message`, or with a message starting with it
// when `prefix`; what it is instead on stderr.
bool is(const vkc::Status& s, Code code, const std::string& message,
        bool prefix = false) {
  if (s.domain() == code &&
      (prefix ? starts_with(s.message(), message) : s.message() == message)) {
    return true;
  }
  std::fprintf(stderr, "got %d \"%s\", wanted %d \"%s\"%s\n",
               static_cast<int>(s.domain()), s.message().c_str(),
               static_cast<int>(code), message.c_str(), prefix ? "..." : "");
  return false;
}

std::unique_ptr<net::Listener> listen_loopback() {
  vkc::Result<std::unique_ptr<net::Listener>> listener =
      net::Listener::create("127.0.0.1", 0);
  if (!listener.ok()) {
    std::fprintf(stderr, "Listener: %s\n", listener.status().message().c_str());
    return nullptr;
  }
  return std::move(listener).value();
}

vkc::UniqueFd dial_to(const net::Listener& listener) {
  vkc::Result<vkc::UniqueFd> fd = net::dial("127.0.0.1", listener.port(), 2s);
  if (!fd.ok()) {
    std::fprintf(stderr, "dial: %s\n", fd.status().message().c_str());
    return {};
  }
  return std::move(fd).value();
}

vkc::UniqueFd accept_from(net::Listener& listener) {
  vkc::Result<vkc::UniqueFd> fd = listener.accept(2s);
  if (!fd.ok()) {
    std::fprintf(stderr, "accept: %s\n", fd.status().message().c_str());
    return {};
  }
  return std::move(fd).value();
}

// --- Listener. ---------------------------------------------------------------

// The receive buffer of a connection accepted from a raw listener on
// loopback with none set, as the kernel reports it.
int default_accepted_receive_buffer() {
  const test::LoopbackListener l = test::bind_loopback();
  const vkc::UniqueFd c = test::connect_loopback(l.port);
  const vkc::UniqueFd accepted = test::accept_within(l.fd.get(), 2s);
  if (!c.valid() || !accepted.valid()) return -1;
  return option(accepted.get(), SOL_SOCKET, SO_RCVBUF);
}

// What a socket given 2 MiB reads back at least: Linux holds what is asked
// for to net.core.rmem_max, then doubles it for its bookkeeping; macOS
// rounds an accepted socket's up, by an amount that varies.
int receive_buffer_floor() {
  int asked = 2 << 20;  // NOLINT(misc-const-correctness): Linux lowers it
#ifdef __linux__
  std::ifstream rmem_max("/proc/sys/net/core/rmem_max");
  int most = 0;
  if (!(rmem_max >> most)) return -1;
  asked = 2 * std::min(asked, most);
#endif
  return asked;
}

int listener_case() {
  using net::Listener;
  for (const char* host : {"", "0.0.0.0", "localhost"}) {
    const vkc::Result<std::unique_ptr<Listener>> refused =
        Listener::create(host, 0);
    CHECK(!refused.ok() && refused.status().domain() == Code::InvalidArgument);
    CHECK(starts_with(refused.status().message(), "Listener::create: "));
  }
  CHECK(Listener::create("", 0).status().message() ==
        "Listener::create: the host is empty");
  CHECK(Listener::create("0.0.0.0", 0).status().message() ==
        "Listener::create: 0.0.0.0 is the wildcard, not one host's address");
  CHECK(Listener::create("localhost", 0).status().message() ==
        "Listener::create: \"localhost\" is not a numeric IPv4 address");

  auto listener = listen_loopback();
  CHECK(listener != nullptr && listener->port() != 0);
  const std::string name = "127.0.0.1:" + std::to_string(listener->port());
  // In use; and not this host's (TEST-NET-1).
  vkc::Result<std::unique_ptr<Listener>> refused =
      Listener::create("127.0.0.1", listener->port());
  CHECK(is(refused.status(), Code::IoError,
           "Listener::create: cannot listen on " + name + ": ", true));
  refused = Listener::create("192.0.2.1", 0);
  CHECK(is(refused.status(), Code::IoError,
           "Listener::create: cannot listen on 192.0.2.1:0: ", true));

  // None waiting: an empty fd, at once or within the wait.
  vkc::Result<vkc::UniqueFd> none = listener->accept(0ms);
  CHECK(none.ok() && !none->valid());
  const Clock::time_point began = Clock::now();
  none = listener->accept(50ms);
  CHECK(none.ok() && !none->valid() && Clock::now() - began >= 45ms);

  // An accepted socket: non-blocking, close-on-exec, and the 2 MiB receive
  // buffer, above the default.
  const int asked = receive_buffer_floor();
  CHECK(asked > 0 && default_accepted_receive_buffer() < asked);
  const vkc::UniqueFd peer = test::connect_loopback(listener->port());
  CHECK(peer.valid());
  const vkc::UniqueFd accepted = accept_from(*listener);
  CHECK(accepted.valid());
  CHECK(nonblocking(accepted.get()) && cloexec(accepted.get()));
  CHECK(option(accepted.get(), SOL_SOCKET, SO_RCVBUF) >= asked);
  CHECK(peer_port(accepted.get()) == test::local_port(peer.get()));

  // Destroyed, it refuses a connection.
  const std::uint16_t port = listener->port();
  listener.reset();
  CHECK(!test::connect_loopback(port).valid());
  return 0;
}

// A connection reset as it waits is never handed out, on either system
// (macOS fails its setup, Linux has no peer to name): accept gives an empty
// fd, and the next connection is accepted, with its peer named and its
// setup made, as a reader that adopts it does.
int listener_reset_case() {
  auto listener = listen_loopback();
  CHECK(listener != nullptr);
  {
    const vkc::UniqueFd reset = test::connect_loopback(listener->port());
    CHECK(reset.valid());
    const linger hard{1, 0};  // its close sends a reset
    CHECK(::setsockopt(reset.get(), SOL_SOCKET, SO_LINGER, &hard,
                       sizeof hard) == 0);
  }
  std::this_thread::sleep_for(20ms);
  const vkc::Result<vkc::UniqueFd> none = listener->accept(200ms);
  CHECK(none.ok() && !none->valid());
  const vkc::UniqueFd next = dial_to(*listener);
  CHECK(next.valid());
  const vkc::UniqueFd accepted = accept_from(*listener);
  CHECK(accepted.valid() &&
        peer_port(accepted.get()) == test::local_port(next.get()));
  CHECK(net::peer_of("test", accepted.get()).ok() &&
        net::prepare("test", accepted.get()).ok());
  return 0;
}

// Out of descriptors, an accept is an IoError naming the listener and the
// system's words, and the listener goes on: the next accept takes the
// connection, which Linux kept queued, or a new one, as macOS drops it.
int listener_out_of_descriptors_case() {
  auto listener = listen_loopback();
  CHECK(listener != nullptr);
  const vkc::UniqueFd peer = test::connect_loopback(listener->port());
  CHECK(peer.valid());
  rlimit saved{};
  CHECK(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
  const int lowest = ::dup(peer.get());  // the lowest free descriptor
  CHECK(lowest >= 0 && ::close(lowest) == 0);
  rlimit low = saved;
  low.rlim_cur = static_cast<rlim_t>(lowest);
  CHECK(::setrlimit(RLIMIT_NOFILE, &low) == 0);
  const vkc::Result<vkc::UniqueFd> failed = listener->accept(2s);
  const bool restored = ::setrlimit(RLIMIT_NOFILE, &saved) == 0;
  CHECK(restored);
  CHECK(is(failed.status(), Code::IoError,
           "Listener: 127.0.0.1:" + std::to_string(listener->port()) +
               ": cannot accept: ",
           true));
  const vkc::UniqueFd next = test::connect_loopback(listener->port());
  CHECK(next.valid());
  const vkc::UniqueFd accepted = accept_from(*listener);
  CHECK(accepted.valid());
#ifdef __linux__
  CHECK(peer_port(accepted.get()) == test::local_port(peer.get()));
#else
  CHECK(peer_port(accepted.get()) == test::local_port(next.get()));
#endif
  return 0;
}

// --- dial. -------------------------------------------------------------------

int dial_case() {
  const auto refusal = [](const std::string& host, std::uint16_t port,
                          std::chrono::milliseconds limit) {
    const vkc::Result<vkc::UniqueFd> fd = net::dial(host, port, limit);
    return fd.ok() || fd.status().domain() != Code::InvalidArgument
               ? std::string("(not refused)")
               : fd.status().message();
  };
  CHECK(refusal("", 7101, 2s) == "dial: the host is empty");
  CHECK(refusal("0.0.0.0", 7101, 2s) ==
        "dial: 0.0.0.0 is the wildcard, not one host's address");
  CHECK(refusal("localhost", 7101, 2s) ==
        "dial: \"localhost\" is not a numeric IPv4 address");
  CHECK(refusal("10.8.0.1 ", 7101, 2s) ==
        "dial: \"10.8.0.1 \" is not a numeric IPv4 address");
  CHECK(refusal("10.8.0.1", 0, 2s) == "dial: port 0 is not a port to dial");
  CHECK(refusal("10.8.0.1", 7101, 0ms) ==
        "dial: a limit of 0 ms; it must be positive");
  CHECK(refusal("10.8.0.1", 7101, -5ms) ==
        "dial: a limit of -5 ms; it must be positive");

  // A closed port: the system's words.
  const std::uint16_t closed = test::closed_port();
  CHECK(closed != 0);
  const vkc::Result<vkc::UniqueFd> refused = net::dial("127.0.0.1", closed, 2s);
  CHECK(is(refused.status(), Code::IoError,
           "dial: 127.0.0.1:" + std::to_string(closed) + ": cannot connect: ",
           true));

  // A Listener: connected, non-blocking and close-on-exec.
  auto listener = listen_loopback();
  CHECK(listener != nullptr);
  const vkc::UniqueFd fd = dial_to(*listener);
  CHECK(fd.valid() && nonblocking(fd.get()) && cloexec(fd.get()));
  CHECK(peer_port(fd.get()) == listener->port());
  const vkc::UniqueFd accepted = accept_from(*listener);
  CHECK(accepted.valid() &&
        peer_port(accepted.get()) == test::local_port(fd.get()));
  return 0;
}

// A connect that never finishes ends at the limit. macOS drops a SYN to a
// socket bound and not listening, which Linux refuses; Linux drops one to a
// listener whose queue is full, which macOS resets. So the port is bound,
// on Linux listening with a queue of one, and connected to until a connect
// does not finish.
int dial_limit_case() {
  const vkc::UniqueFd listener(::socket(AF_INET, SOCK_STREAM, 0));
  const sockaddr_in a = test::loopback(0);
  CHECK(listener.valid() &&
        ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&a),
               sizeof a) == 0);
#ifndef __APPLE__
  CHECK(::listen(listener.get(), 0) == 0);
#endif
  const std::uint16_t port = test::local_port(listener.get());
  std::vector<vkc::UniqueFd> queued;
  bool full = false;
  for (int i = 0; i < 64 && !full; ++i) {
    vkc::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(fd.valid() && ::fcntl(fd.get(), F_SETFL, O_NONBLOCK) == 0);
    const sockaddr_in to = test::loopback(port);
    // NOLINTNEXTLINE(bugprone-unused-return-value): the poll reads its outcome
    (void)::connect(fd.get(), reinterpret_cast<const sockaddr*>(&to),
                    sizeof to);
    pollfd p{fd.get(), POLLOUT, 0};
    full = ::poll(&p, 1, 100) == 0;
    queued.push_back(std::move(fd));
  }
  CHECK(full);
  const Clock::time_point began = Clock::now();
  const vkc::Result<vkc::UniqueFd> fd = net::dial("127.0.0.1", port, 300ms);
  const auto took = Clock::now() - began;
  CHECK(is(fd.status(), Code::IoError,
           "dial: 127.0.0.1:" + std::to_string(port) +
               ": cannot connect within 300 ms"));
  CHECK(took >= 300ms && took < 2s);
  return 0;
}

}  // namespace

// libc++ throws from std::vector's allocation inline, where clang-tidy sees
// it; a test that throws fails, as it should.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  if (listener_case() != 0) return 1;
  if (listener_reset_case() != 0) return 1;
  if (listener_out_of_descriptors_case() != 0) return 1;
  if (dial_case() != 0) return 1;
  if (dial_limit_case() != 0) return 1;
  std::puts("net dial: OK");
  return 0;
}
