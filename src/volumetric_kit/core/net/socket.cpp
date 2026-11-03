// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/net/socket.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>  // IWYU pragma: keep (ssize_t on Linux)
#ifndef __APPLE__
#include <linux/sockios.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#ifdef __linux__
#include <atomic>
#include <fstream>

#include "volumetric_kit/core/base/log.hpp"
#endif

#include "socket_internal.hpp"
#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"
#include "volumetric_kit/core/net/address.hpp"

namespace volumetric_kit::core::net {
namespace {

#ifdef __APPLE__
constexpr int kSendFlags = 0;  // SO_NOSIGPIPE is set on every socket
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

// Connections the kernel queues before the accept thread takes them.
constexpr int kBacklog = 8;
// One send or recv moves at most this much: macOS refuses a count above
// INT_MAX.
constexpr std::size_t kMaxIo = std::size_t{1} << 30;

// "<api>: <what>: <the system's reason>": OutOfMemory for the kernel's
// memory, IoError for the rest.
Status failure(std::string_view api, std::string_view what, int error) {
  std::string message(api);
  message += ": ";
  message += what;
  message += ": ";
  message += std::generic_category().message(error);
  if (error == ENOMEM || error == ENOBUFS) {
    return Status::out_of_memory(std::move(message));
  }
  return Status::io_error(std::move(message));
}

Status set_option(std::string_view api, int fd, int level, int name, int value,
                  std::string_view what) {
  if (::setsockopt(fd, level, name, &value, sizeof value) != 0) {
    return failure(api, what, errno);
  }
  return {};
}

Result<UniqueFd> new_socket(std::string_view api) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) return failure(api, "cannot make a socket", errno);
  VKC_TRY(prepare(api, fd.get()));
  return fd;
}

const sockaddr* as_sockaddr(const sockaddr_in& in) {
  return reinterpret_cast<const sockaddr*>(&in);
}

#ifdef __linux__
// The source the tier's diagnostics carry; the default sink prints
// `[net <level>]`.
constexpr std::string_view kLogSource = "net";

constexpr char kAllowedCongestion[] =
    "/proc/sys/net/ipv4/tcp_allowed_congestion_control";

// The algorithms tcp_allowed_congestion_control lists, read at each accept
// so a change to it reaches the next viewer; empty if it cannot be read.
std::string allowed_congestion() {
  std::ifstream file(kAllowedCongestion);
  std::string text;
  std::getline(file, text);
  return text;
}

// Set by the first socket of the process that keeps the default.
std::atomic<bool> kept_default_said{false};
#endif

}  // namespace

std::chrono::steady_clock::time_point deadline_after(
    std::chrono::steady_clock::time_point from,
    std::chrono::milliseconds limit) {
  using Clock = std::chrono::steady_clock;
  if (limit <= std::chrono::milliseconds{0}) return from;
  // Compared in milliseconds: the limit in the clock's nanoseconds may not
  // fit.
  if (limit >= std::chrono::floor<std::chrono::milliseconds>(
                   Clock::time_point::max() - from)) {
    return Clock::time_point::max();
  }
  return from + limit;
}

Status prepare(std::string_view api, int fd) try {
  const int fd_flags = ::fcntl(fd, F_GETFD);
  if (fd_flags < 0 || ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0) {
    return failure(api, "cannot set FD_CLOEXEC", errno);
  }
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return failure(api, "cannot set O_NONBLOCK", errno);
  }
#ifdef __APPLE__
  VKC_TRY(set_option(api, fd, SOL_SOCKET, SO_NOSIGPIPE, 1,
                     "cannot set SO_NOSIGPIPE"));
#endif
  return {};
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

namespace detail {

bool lists(std::string_view allowed, std::string_view name) {
  constexpr std::string_view kSpace = " \t\n\v\f\r";
  for (std::size_t at = allowed.find_first_not_of(kSpace);
       at != std::string_view::npos;) {
    const std::size_t end =
        std::min(allowed.find_first_of(kSpace, at), allowed.size());
    if (allowed.substr(at, end - at) == name) return true;
    at = allowed.find_first_not_of(kSpace, end);
  }
  return false;
}

#ifdef __linux__
bool prefer_congestion(std::string_view api, int fd, std::string_view allowed) {
  int error = 0;
  if (lists(allowed, kCongestion)) {
    if (::setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, kCongestion.data(),
                     static_cast<socklen_t>(kCongestion.size())) == 0) {
      return true;
    }
    error = errno;
  }
  if (!kept_default_said.exchange(true)) {
    try {
      std::string message(api);
      message += ": viewers keep the kernel's congestion control, not ";
      message += kCongestion;
      message += ": ";
      if (error != 0) {
        message += "cannot set TCP_CONGESTION: ";
        message += std::generic_category().message(error);
      } else {
        message += "not in ";
        message += kAllowedCongestion;
      }
      log_message(LogLevel::Info, kLogSource, message);
      // A note that cannot be made is not said: the socket keeps the
      // default either way.
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
  }
  return false;
}
#endif

}  // namespace detail

Result<UniqueFd> listen_on(std::string_view api, const Endpoint& at,
                           int receive_buffer) try {
  VKC_ASSIGN(UniqueFd fd, new_socket(api));
  VKC_TRY(set_option(api, fd.get(), SOL_SOCKET, SO_REUSEADDR, 1,
                     "cannot set SO_REUSEADDR"));
  if (receive_buffer > 0) {
    VKC_TRY(set_option(api, fd.get(), SOL_SOCKET, SO_RCVBUF, receive_buffer,
                       "cannot set SO_RCVBUF"));
  }
  const sockaddr_in in = detail::to_sockaddr(at);
  if (::bind(fd.get(), as_sockaddr(in), sizeof in) != 0 ||
      ::listen(fd.get(), kBacklog) != 0) {
    const int error = errno;  // before the message's allocations
    return failure(api, "cannot listen on " + to_string(at), error);
  }
  return fd;
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<std::uint16_t> bound_port(std::string_view api, int fd) try {
  sockaddr_in in{};
  socklen_t size = sizeof in;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&in), &size) != 0) {
    return failure(api, "cannot name the socket", errno);
  }
  return ntohs(in.sin_port);
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<Endpoint> peer_of(std::string_view api, int fd) try {
  int type = 0;
  socklen_t type_size = sizeof type;
  if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0) {
    const int error = errno;
    if (error == ENOTSOCK || error == EBADF) {
      return Status::invalid_argument(std::string(api) + ": not a socket");
    }
    return failure(api, "cannot read the socket's type", error);
  }
  if (type != SOCK_STREAM) {
    return Status::invalid_argument(std::string(api) + ": not a stream socket");
  }
  sockaddr_storage local{};
  socklen_t local_size = sizeof local;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_size) !=
      0) {
    return failure(api, "cannot name the socket", errno);
  }
  if (local.ss_family != AF_INET) {
    return Status::invalid_argument(std::string(api) + ": not an IPv4 socket");
  }
  sockaddr_in peer{};
  socklen_t size = sizeof peer;
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &size) != 0) {
    return failure(api, "cannot name the peer", errno);
  }
  return detail::to_endpoint(peer);
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Status set_up_sender(std::string_view api, int fd, const Endpoint& peer) try {
  VKC_TRY(set_option(api, fd, IPPROTO_TCP, TCP_NODELAY, 1,
                     "cannot set TCP_NODELAY"));
  VKC_TRY(set_option(api, fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT,
                     detail::kSendLowWater, "cannot set TCP_NOTSENT_LOWAT"));
#ifdef __linux__
  // Loopback has no loss or long round trip, and keeps the default.
  if (!detail::is_loopback(peer)) {
    (void)detail::prefer_congestion(api, fd, allowed_congestion());
  }
#else
  (void)peer;
#endif
  return {};
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

namespace detail {

Result<UniqueFd> accept_one(std::string_view api, int listener,
                            Endpoint* peer) {
  for (;;) {
    sockaddr_in in{};
    socklen_t size = sizeof in;
    UniqueFd fd(::accept(listener, reinterpret_cast<sockaddr*>(&in), &size));
    *peer = to_endpoint(in);
    if (!fd.valid()) {
      switch (errno) {
        case EINTR:
          continue;
        // None waiting, or one that failed in the queue: Linux passes a
        // pending network error up through accept.
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case ECONNABORTED:
        case EPROTO:
        case ENETDOWN:
        case ENETUNREACH:
        case EHOSTUNREACH:
#ifdef EHOSTDOWN
        case EHOSTDOWN:
#endif
#ifdef ENONET
        case ENONET:
#endif
        case ENOPROTOOPT:
        case EOPNOTSUPP:
          return UniqueFd();
        default:
          return failure(api, "cannot accept", errno);
      }
    }
    // One that cannot be prepared, or has no peer left, failed after it was
    // queued: macOS shows one its peer reset as EINVAL at its first
    // setsockopt, Linux only as ENOTCONN when it is named. It goes too.
    sockaddr_storage named{};
    socklen_t named_size = sizeof named;
    if (!prepare(api, fd.get()).ok() ||
        ::getpeername(fd.get(), reinterpret_cast<sockaddr*>(&named),
                      &named_size) != 0) {
      return UniqueFd();
    }
    return fd;
  }
}

}  // namespace detail

Result<UniqueFd> accept_next(std::string_view api, int listener,
                             Endpoint* peer) try {
  VKC_ASSIGN(UniqueFd fd, detail::accept_one(api, listener, peer));
  if (fd.valid() && !set_up_sender(api, fd.get(), *peer).ok()) {
    return UniqueFd();
  }
  return fd;
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<UniqueFd> start_connect(std::string_view api, const Endpoint& to,
                               bool* connected, int receive_buffer) try {
  VKC_ASSIGN(UniqueFd fd, new_socket(api));
  if (receive_buffer > 0) {
    VKC_TRY(set_option(api, fd.get(), SOL_SOCKET, SO_RCVBUF, receive_buffer,
                       "cannot set SO_RCVBUF"));
  }
  const sockaddr_in in = detail::to_sockaddr(to);
  *connected = ::connect(fd.get(), as_sockaddr(in), sizeof in) == 0;
  const int error = errno;  // before the message's allocations
  // Interrupted, a non-blocking connect goes on as one in progress does.
  if (!*connected && error != EINPROGRESS && error != EINTR) {
    return failure(api, "cannot connect", error);
  }
  return fd;
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Status finish_connect(std::string_view api, int fd) try {
  int error = 0;
  socklen_t size = sizeof error;
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
    error = errno;
  }
  if (error != 0) return failure(api, "cannot connect", error);
  return {};
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

namespace detail {

Result<UniqueFd> connect_within(std::string_view api, const Endpoint& to,
                                std::chrono::milliseconds limit) {
  bool connected = false;
  VKC_ASSIGN(UniqueFd fd, start_connect(api, to, &connected, 0));
  if (connected) return fd;
  VKC_ASSIGN(const short ready, wait_for(api, fd.get(), POLLOUT, limit));
  if (ready == 0) {
    return Status::io_error(std::string(api) + ": cannot connect within " +
                            std::to_string(limit.count()) + " ms");
  }
  VKC_TRY(finish_connect(api, fd.get()));
  return fd;
}

}  // namespace detail

Result<short> wait_for(std::string_view api, int fd, short events,
                       std::chrono::milliseconds timeout) try {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point deadline = deadline_after(Clock::now(), timeout);
  for (;;) {
    pollfd p{fd, events, 0};
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    // A poll waits at most INT_MAX ms (24 days), then the next goes on.
    const std::int64_t ms = std::clamp<std::int64_t>(
        left.count(), 0, std::numeric_limits<int>::max());
    const int n = ::poll(&p, 1, static_cast<int>(ms));
    if (n > 0) return p.revents;
    if (n == 0 && Clock::now() >= deadline) return short{0};
    if (n < 0 && errno != EINTR) return failure(api, "cannot poll", errno);
  }
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<std::size_t> send_some(std::string_view api, int fd,
                              const std::uint8_t* data, std::size_t size) try {
  for (;;) {
    const ssize_t n = ::send(fd, data, std::min(size, kMaxIo), kSendFlags);
    if (n >= 0) return static_cast<std::size_t>(n);
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return std::size_t{0};
    return failure(api, "cannot send", errno);
  }
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<std::size_t> unacknowledged(std::string_view api, int fd) try {
  int bytes = 0;
#ifdef __APPLE__
  socklen_t size = sizeof bytes;
  const int failed = ::getsockopt(fd, SOL_SOCKET, SO_NWRITE, &bytes, &size);
#else
  const int failed = ::ioctl(fd, SIOCOUTQ, &bytes);
#endif
  if (failed != 0) return failure(api, "cannot read the send queue", errno);
  return static_cast<std::size_t>(std::max(bytes, 0));
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<Received> receive_some(std::string_view api, int fd, std::uint8_t* out,
                              std::size_t size) try {
  for (;;) {
    const ssize_t n = ::recv(fd, out, std::min(size, kMaxIo), 0);
    if (n > 0) return Received{static_cast<std::size_t>(n), false};
    if (n == 0) return Received{0, true};
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Received{};
    return failure(api, "cannot receive", errno);
  }
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

}  // namespace volumetric_kit::core::net
