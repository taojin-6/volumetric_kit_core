// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file net/socket.hpp
/// @brief The TCP socket calls a sender and a reader are built from.
///
/// Every socket these calls make or set up is non-blocking and close-on-exec,
/// set explicitly (accepted ones included: Linux does not inherit the flags,
/// and macOS has no accept4). A send through @ref send_some never raises
/// SIGPIPE: SO_NOSIGPIPE is set on every Apple socket, and elsewhere each
/// send passes MSG_NOSIGNAL, so a plain write() there still can. Every call
/// is retried on EINTR: an application's signal handler, as one for Ctrl+C,
/// interrupts poll, which is never restarted. IPv4 stream sockets only. Each
/// call takes `api`, the caller's name, and every failure reads
/// "<api>: <what failed>: <the system's reason>": `OutOfMemory` for the
/// kernel's memory (ENOMEM, ENOBUFS), `InvalidArgument` where named below,
/// `IoError` otherwise.
///
/// Thread safety: the calls hold no state but one flag that is set once per
/// process (the congestion-control note at @ref set_up_sender). Two threads
/// may call them at once on different sockets. One socket is used by one
/// thread at a time.
///
/// @code
/// // A reader's connect, waiting at most 2 s; Ctrl+C is seen between waits.
/// bool connected = false;
/// VKC_ASSIGN(UniqueFd fd, net::start_connect("viewer", to, &connected));
/// if (!connected) {
///   VKC_ASSIGN(const short ready, net::wait_for("viewer", fd.get(), POLLOUT,
///                                               std::chrono::seconds{2}));
///   if (ready == 0) return Status::io_error("viewer: no answer");
///   VKC_TRY(net::finish_connect("viewer", fd.get()));
/// }
/// @endcode

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"
#include "volumetric_kit/core/net/address.hpp"
#include "volumetric_kit/core/net/export.hpp"

namespace volumetric_kit::core::net {

/// The longest one wait lasts in a loop that must see a stop (a sender's
/// threads): within it, with no shutdown semantics that differ by OS.
inline constexpr std::chrono::milliseconds kPollSlice{20};
/// A reader's receive buffer, set before the connect so the window scale
/// fits it: the most a stream carries in a round trip over the VPN. Linux
/// holds it to net.core.rmem_max.
inline constexpr int kReceiveBuffer = 2 << 20;

/// @brief A deadline @p limit after @p from.
/// @param from   The start.
/// @param limit  How long after it.
/// @return @p from + @p limit, saturating: `milliseconds::max()` gives the
///         clock's end ("never"), and a limit of 0 or less gives @p from.
VKC_NET_API std::chrono::steady_clock::time_point deadline_after(
    std::chrono::steady_clock::time_point from,
    std::chrono::milliseconds limit);

/// @brief Make @p fd close-on-exec and non-blocking, and on Apple set
///        SO_NOSIGPIPE: for a socket this tier did not make.
/// @param api  The caller's name.
/// @param fd   A socket.
/// @return OK, or the call that failed.
VKC_NET_API Status prepare(std::string_view api, int fd);

/// @brief The peer of a connected socket.
/// @param api  The caller's name.
/// @param fd   A connected IPv4 stream socket.
/// @return The peer of @p fd; `InvalidArgument` "<api>: not a socket", "…:
///         not a stream socket" or "…: not an IPv4 socket"; `IoError`
///         "<api>: cannot name the peer: …" for one not connected, or reset
///         already.
VKC_NET_API Result<Endpoint> peer_of(std::string_view api, int fd);

/// @brief Set @p fd, connected to @p peer, up to send: TCP_NODELAY,
///        TCP_NOTSENT_LOWAT at 128 KiB, and on Linux, for a peer off
///        loopback, BBR when /proc/sys/net/ipv4/tcp_allowed_congestion_control
///        lists it. Otherwise the kernel's default is kept, and the process's
///        first such socket says so at Info, source "net".
/// @param api   The caller's name.
/// @param fd    A connected socket.
/// @param peer  Its peer, as @ref peer_of or @ref accept_next names it.
/// @return OK; `IoError` for a socket that cannot be set up, such as one its
///         peer reset.
VKC_NET_API Status set_up_sender(std::string_view api, int fd,
                                 const Endpoint& peer);

/// @brief A socket listening on @p at, the wildcard included, with
///        SO_REUSEADDR so a restarted sender takes its port back at once.
/// @param api             The caller's name.
/// @param at              The address; port 0 takes a free one
///                        (@ref bound_port).
/// @param receive_buffer  When positive, set before the listen, so that each
///                        accepted socket inherits it and its window scale.
/// @return The socket; `IoError` "<api>: cannot listen on a.b.c.d:port: …".
VKC_NET_API Result<UniqueFd> listen_on(std::string_view api, const Endpoint& at,
                                       int receive_buffer = 0);

/// @brief The port a socket is bound to.
/// @param api  The caller's name.
/// @param fd   A bound socket.
/// @return The port @p fd is bound to.
VKC_NET_API Result<std::uint16_t> bound_port(std::string_view api, int fd);

/// @brief The next connection waiting on @p listener, prepared and set up
///        to send, and its peer, for a sender's accept loop.
/// @param api       The caller's name.
/// @param listener  A socket from @ref listen_on.
/// @param[out] peer The connection's peer.
/// @return The socket; an empty one when none is waiting, or when one failed
///         before it was taken or cannot be set up, which is closed;
///         `IoError` / `OutOfMemory` for an accept that fails otherwise, such
///         as out of descriptors. Linux keeps that connection queued, so the
///         next accept fails at once; macOS drops it.
VKC_NET_API Result<UniqueFd> accept_next(std::string_view api, int listener,
                                         Endpoint* peer);

/// @brief Start connecting to @p to without waiting.
/// @param api             The caller's name.
/// @param to              The address to connect to.
/// @param[out] connected  Whether it connected already; else wait for
///                        POLLOUT and call @ref finish_connect.
/// @param receive_buffer  Set before the connect when positive.
/// @return The socket; `IoError` "<api>: cannot connect: …".
VKC_NET_API Result<UniqueFd> start_connect(std::string_view api,
                                           const Endpoint& to, bool* connected,
                                           int receive_buffer = kReceiveBuffer);

/// @brief The outcome of @ref start_connect's connect.
/// @param api  The caller's name.
/// @param fd   The socket, once it polls writable.
/// @return OK; `IoError` "<api>: cannot connect: …".
VKC_NET_API Status finish_connect(std::string_view api, int fd);

/// @brief Wait up to @p timeout for @p events (POLLIN, POLLOUT) on @p fd.
/// @param api      The caller's name.
/// @param fd       The socket.
/// @param events   What to wait for, as poll's events.
/// @param timeout  The longest to wait; 0 or less looks once.
/// @return The events that came; 0 when none did by then.
VKC_NET_API Result<short> wait_for(std::string_view api, int fd, short events,
                                   std::chrono::milliseconds timeout);

/// @brief Send what @p fd takes now, without waiting.
/// @param api   The caller's name.
/// @param fd    A connected socket.
/// @param data  The bytes.
/// @param size  How many.
/// @return The bytes of @p size at @p data that @p fd took now, at most
///         1 GiB; 0 when it takes none; `IoError` "<api>: cannot send: …".
VKC_NET_API Result<std::size_t> send_some(std::string_view api, int fd,
                                          const std::uint8_t* data,
                                          std::size_t size);

/// @brief The bytes sent on @p fd that its peer has not acknowledged,
///        unsent ones included (SO_NWRITE on Apple, SIOCOUTQ elsewhere): a
///        sender's stall rule.
/// @param api  The caller's name.
/// @param fd   A connected socket.
/// @return The bytes.
VKC_NET_API Result<std::size_t> unacknowledged(std::string_view api, int fd);

/// @brief What one receive found.
///
/// @code
/// VKC_ASSIGN(const net::Received got,
///            net::receive_some("viewer", fd, buffer, sizeof buffer));
/// if (got.closed) return Status::io_error("viewer: the sender left");
/// @endcode
struct Received {
  std::size_t bytes = 0;  ///< Read; 0 when none had come.
  bool closed = false;    ///< The peer closed its side.
};

/// @brief Receive what has come, without waiting.
/// @param api   The caller's name.
/// @param fd    A connected socket.
/// @param out   Where the bytes go.
/// @param size  The most to read.
/// @return What had come on @p fd, up to @p size bytes, read without
///         waiting; `IoError` "<api>: cannot receive: …".
VKC_NET_API Result<Received> receive_some(std::string_view api, int fd,
                                          std::uint8_t* out, std::size_t size);

}  // namespace volumetric_kit::core::net
