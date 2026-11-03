// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file net_loopback.hpp
/// @brief Raw sockets on 127.0.0.1 for a test that is a `main()` of its own
///        (`volumetric_kit::core_test_base`), to play a sender or a reader
///        with any bytes.
///
/// Always 127.0.0.1 and port 0, never 0.0.0.0, which could raise a firewall
/// prompt. The sockets are blocking, as the system makes them: each helper
/// is the plain system call a test checks a socket call against, so none
/// goes through the net tier. A failure gives an empty or zero result.
///
/// @code
/// const test::LoopbackListener listener = test::bind_loopback();
/// const vkc::UniqueFd reader = test::connect_loopback(listener.port);
/// const vkc::UniqueFd sender = test::accept_within(listener.fd.get(), 2s);
/// CHECK(test::send_bytes(sender.get(), {1, 2, 3}));
/// std::vector<std::uint8_t> got;
/// test::read_upto(reader.get(), 3, 2s, &got);  // got: 1, 2, 3
/// @endcode

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>  // IWYU pragma: keep (ssize_t on Linux)

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "volumetric_kit/core/base/unique_fd.hpp"

namespace volumetric_kit::core::test {

/// @return 127.0.0.1:@p port as the system's socket address.
inline sockaddr_in loopback(std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

/// @return The port @p fd is bound to; 0 if it cannot be named.
inline std::uint16_t local_port(int fd) {
  sockaddr_in a{};
  socklen_t size = sizeof a;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) != 0) return 0;
  return ntohs(a.sin_port);
}

/// @brief A blocking socket listening on 127.0.0.1, on a free port.
///
/// @code
/// const test::LoopbackListener l = test::bind_loopback();
/// if (!l.fd.valid()) return 1;  // the reason is on stderr
/// @endcode
struct LoopbackListener {
  UniqueFd fd;             ///< Empty if it could not listen.
  std::uint16_t port = 0;  ///< The port it took; 0 if none.
};

/// @return A socket listening on 127.0.0.1, on a free port; an empty one,
///         the reason on stderr, if it cannot listen.
inline LoopbackListener bind_loopback() {
  LoopbackListener l;
  l.fd.reset(::socket(AF_INET, SOCK_STREAM, 0));
  const sockaddr_in a = loopback(0);
  if (!l.fd.valid() ||
      ::bind(l.fd.get(), reinterpret_cast<const sockaddr*>(&a), sizeof a) !=
          0 ||
      ::listen(l.fd.get(), 8) != 0) {
    std::perror("bind_loopback");
    return {};
  }
  l.port = local_port(l.fd.get());
  return l;
}

/// @return A port on 127.0.0.1 that was free a moment ago, its listener
///         closed, so a connect to it is refused. (One bound and not
///         listening is no use: macOS drops a SYN to it, where Linux refuses
///         it.)
inline std::uint16_t closed_port() { return bind_loopback().port; }

/// @return The next connection on @p listener within @p timeout, or none.
inline UniqueFd accept_within(int listener, std::chrono::milliseconds timeout) {
  pollfd p{listener, POLLIN, 0};
  if (::poll(&p, 1, static_cast<int>(timeout.count())) != 1) return {};
  return UniqueFd(::accept(listener, nullptr, nullptr));
}

/// @return A blocking connection to 127.0.0.1:@p port, its receive buffer
///         @p receive_buffer bytes when nonzero; none if it fails.
inline UniqueFd connect_loopback(std::uint16_t port, int receive_buffer = 0) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) return {};
  if (receive_buffer > 0) {
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                 sizeof receive_buffer);
  }
  const sockaddr_in a = loopback(port);
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&a), sizeof a) !=
      0) {
    return {};
  }
  return fd;
}

/// @return Whether all of @p bytes went out on @p fd.
inline bool send_bytes(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::send(fd, bytes.data() + done, bytes.size() - done, 0);
    if (n <= 0) return false;
    done += static_cast<std::size_t>(n);
  }
  return true;
}

/// @brief Read from @p fd until @p out holds @p size bytes, the peer's end
///        (`*eof`), or @p timeout.
/// @param fd       A connected socket.
/// @param size     The bytes @p out should hold.
/// @param timeout  The longest to read.
/// @param out      Where the bytes are appended.
/// @param eof      Set to whether the peer's end was read, when not null.
inline void read_upto(int fd, std::size_t size,
                      std::chrono::milliseconds timeout,
                      std::vector<std::uint8_t>* out, bool* eof = nullptr) {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point end = Clock::now() + timeout;
  std::uint8_t buffer[65536];
  if (eof != nullptr) *eof = false;
  while (out->size() < size) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - Clock::now());
    if (left.count() <= 0) return;
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(left.count())) != 1) continue;
    const std::size_t want = std::min(sizeof buffer, size - out->size());
    const ssize_t n = ::recv(fd, buffer, want, 0);
    if (n <= 0) {
      if (eof != nullptr) *eof = true;
      return;
    }
    out->insert(out->end(), buffer, buffer + n);
  }
}

}  // namespace volumetric_kit::core::test
