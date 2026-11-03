// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file net/address.hpp
/// @brief IPv4 endpoints, and the checks that a typed address, a config field
///        and a socket call make of a host and a port.
///
/// IPv4 alone: a stream crosses a routed VPN whose hosts have A records only,
/// and an IPv6 address could only be refused, or stall a connect where IPv6
/// is broken. Each refusal's message is a sentence an address entry can show
/// as it is; a caller adds its own name with `Status::with_context`.
/// Every function here is thread-safe.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/net/export.hpp"

namespace volumetric_kit::core::net {

/// @brief An IPv4 address and a port, as a socket binds or connects to them.
///
/// Host byte order, so it names no system socket type.
///
/// @code
/// const Endpoint lab{0x0A080001, 7101};  // 10.8.0.1:7101
/// assert(to_string(lab) == "10.8.0.1:7101");
/// @endcode
struct Endpoint {
  /// Host byte order: 127.0.0.1 is 0x7F000001; 0 is 0.0.0.0.
  std::uint32_t ipv4 = 0;
  /// To listen on, 0 takes a free port.
  std::uint16_t port = 0;
};

/// @brief Read a numeric IPv4 address.
/// @param host  The text: dotted decimal, read as `inet_pton` reads it.
/// @param port  The port the endpoint takes.
/// @return @p host as a numeric IPv4 address in dotted decimal, the wildcard
///         0.0.0.0 included, with @p port; nullopt for anything else: a name,
///         IPv6, or a part over 255 or with a leading zero (`inet_pton`).
VKC_NET_API std::optional<Endpoint> parse_ipv4(std::string_view host,
                                               std::uint16_t port);

/// @brief One host's address, as @ref dial connects to and a @ref Listener
///        listens on. Never the wildcard, which names every interface to
///        listen on and no host to connect to.
/// @param host  A numeric IPv4 address.
/// @param port  Any port, 0 included: the caller says what 0 means.
/// @return The endpoint; `InvalidArgument` "the host is empty",
///         "\"<host>\" is not a numeric IPv4 address" or "0.0.0.0 is the
///         wildcard, not one host's address".
VKC_NET_API Result<Endpoint> parse_endpoint(std::string_view host,
                                            std::uint16_t port);

/// @return "a.b.c.d:port".
VKC_NET_API std::string to_string(const Endpoint& endpoint);

/// @brief Every IPv4 address @p host resolves to for TCP (`getaddrinfo`),
///        with @p port. Blocks for as long as the resolver takes; a caller
///        that must not block calls it from a thread of its own.
/// @param api   The caller's name, which starts every message.
/// @param host  A name, or a numeric address.
/// @param port  The port each endpoint takes.
/// @return At least one endpoint, in the resolver's order; `NotFound`
///         "<api>: cannot look up the name: <the resolver's words>" for a
///         name that does not resolve, or "…: no IPv4 address for TCP";
///         `OutOfMemory` for the resolver's memory; `IoError` otherwise.
VKC_NET_API Result<std::vector<Endpoint>> resolve(std::string_view api,
                                                  const std::string& host,
                                                  std::uint16_t port);

}  // namespace volumetric_kit::core::net
