// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

// Internal to core_net: an Endpoint as the system's sockaddr_in, which no
// public header names, the sender's socket options and congestion-control
// choice, and the two calls dial.cpp builds on. Not installed.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <chrono>
#include <string_view>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"
#include "volumetric_kit/core/net/address.hpp"
#include "volumetric_kit/core/net/export.hpp"

namespace volumetric_kit::core::net::detail {

// What a sender's socket may hold unsent before poll stops calling it
// writable (TCP_NOTSENT_LOWAT): a viewer takes its next record only below it.
inline constexpr int kSendLowWater = 128 << 10;
// The congestion control a sender's socket to a peer that is not loopback
// asks for, on Linux (TCP_CONGESTION): at a long round trip CUBIC's window
// collapses on loss and decays while a stream idles between frames; BBR's
// does neither (grid's DECISIONS.md, 2026-11-01 networking).
inline constexpr std::string_view kCongestion = "bbr";

// `endpoint` as the system's socket address.
inline sockaddr_in to_sockaddr(const Endpoint& endpoint) {
  sockaddr_in in{};
  in.sin_family = AF_INET;
  in.sin_port = htons(endpoint.port);
  in.sin_addr.s_addr = htonl(endpoint.ipv4);
  return in;
}

// The endpoint a system socket address names.
inline Endpoint to_endpoint(const sockaddr_in& in) {
  return Endpoint{ntohl(in.sin_addr.s_addr), ntohs(in.sin_port)};
}

// Whether `endpoint` is IPv4 loopback, 127.0.0.0/8.
inline bool is_loopback(const Endpoint& endpoint) {
  return (endpoint.ipv4 >> 24) == 127;
}

// Whether `allowed`, names separated by white space as
// /proc/sys/net/ipv4/tcp_allowed_congestion_control holds them, lists `name`
// as a whole word. Exported for the tests alone, which reach this header.
VKC_NET_API bool lists(std::string_view allowed, std::string_view name);

#ifdef __linux__
// Set `fd` to kCongestion if `allowed`, the text of
// tcp_allowed_congestion_control, lists it: whether it did. Linux limits a
// process without CAP_NET_ADMIN to the algorithms listed there; consulting
// the list here makes one with it, as root, behave as every other.
// Otherwise, or if the kernel refuses, `fd` keeps the kernel's default, and
// the first such socket of a process says so at Info, naming `api`.
// Exported for the tests alone, which reach this header.
VKC_NET_API bool prefer_congestion(std::string_view api, int fd,
                                   std::string_view allowed);
#endif

// The next connection waiting on `listener`, prepared, and its peer; an
// empty fd when none is waiting, or one failed before it was taken, cannot
// be prepared or has no peer left (reset as it waited), which is closed.
// IoError (OutOfMemory for the kernel's memory) for an accept that fails
// otherwise, as out of descriptors: Linux keeps that connection queued, and
// macOS drops it.
Result<UniqueFd> accept_one(std::string_view api, int listener, Endpoint* peer);

// A socket connected to `to` within `limit`, which is positive, its receive
// buffer the kernel's: start_connect, a wait, then finish_connect. IoError
// "<api>: cannot connect: <the system's reason>", or "<api>: cannot connect
// within N ms" past `limit`.
Result<UniqueFd> connect_within(std::string_view api, const Endpoint& to,
                                std::chrono::milliseconds limit);

}  // namespace volumetric_kit::core::net::detail
