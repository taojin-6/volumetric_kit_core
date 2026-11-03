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
/// @param host  The text: dotted decimal, read as glibc's `inet_pton` reads
///              it on every platform.
/// @param port  The port the endpoint takes.
/// @return @p host as a numeric IPv4 address in dotted decimal, the wildcard
///         0.0.0.0 included, with @p port; nullopt for anything else: a name,
///         IPv6, or a part over 255 or with a leading zero, which Apple's
///         `inet_pton` would read as decimal and `inet_aton` as octal.
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
///        that must not block calls it from a thread of its own. A host the
///        system may read as a number must be dotted decimal as
///        @ref parse_ipv4 reads it, and not the wildcard, and is read without
///        the resolver: digits and dots alone; a form `inet_aton` takes
///        ("10.1", octal "010.8.0.1", hex "0x7f.0.0.1"); or anything with a
///        ':', IPv6, which glibc's `getaddrinfo` reads as IPv4 when mapped
///        ("::ffff:0.0.0.0" as 0.0.0.0). A name's addresses are the
///        resolver's.
/// @param api   The caller's name, which starts every message.
/// @param host  A name, or a numeric IPv4 address.
/// @param port  The port each endpoint takes.
/// @return At least one endpoint, in the resolver's order (a numeric host's
///         own one); before any lookup, `InvalidArgument` "<api>: the host
///         is empty", "<api>: the host is not a numeric IPv4 address: four
///         decimal parts from 0 to 255, with no leading zero" or "<api>:
///         0.0.0.0 is the wildcard, not one host's address"; `NotFound`
///         "<api>: cannot look up the name: <the resolver's words>" for a
///         name that does not resolve, or "…: no IPv4 address for TCP";
///         `OutOfMemory` for the resolver's memory; `IoError` otherwise.
VKC_NET_API Result<std::vector<Endpoint>> resolve(std::string_view api,
                                                  const std::string& host,
                                                  std::uint16_t port);

/// @brief A port as typed: decimal digits alone, 1 to 65535, with no
///        leading zero, as an address part has none.
/// @param text  The port's text: no sign, space or other character.
/// @return The port; `InvalidArgument` "\"<text>\" is not a port: a number
///         from 1 to 65535, with no leading zero".
VKC_NET_API Result<std::uint16_t> parse_port(std::string_view text);

/// @brief A host to reach, by name or numeric IPv4 address, and its port.
///
/// @code
/// const Result<HostPort> lab = parse_host_port("lab-box:7101", 47801);
/// assert(lab->host == "lab-box" && lab->port == 7101);
/// @endcode
struct HostPort {
  /// A name or a numeric address, as typed.
  std::string host;
  /// The port given, or the caller's default.
  std::uint16_t port = 0;
};

/// @brief "host" or "host:port", as a command line or one address field
///        gives it. The host is checked no further: a name is looked up when
///        connected to, and where one host's numeric address is meant,
///        @ref parse_endpoint checks it.
/// @param text          The address as typed.
/// @param default_port  The port when @p text names none; 0 requires one.
/// @return The host and port; `InvalidArgument` "\"<text>\" names no host",
///         "\"<text>\" names no port" (with @p default_port 0), "\"<text>\"
///         is IPv6, which is not supported" (a leading '['), "\"<text>\" has
///         a second ':'; IPv6 is not supported", or @ref parse_port's reason.
VKC_NET_API Result<HostPort> parse_host_port(std::string_view text,
                                             std::uint16_t default_port);

}  // namespace volumetric_kit::core::net
