// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/net/address.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "socket_internal.hpp"
#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/base/result.hpp"

namespace volumetric_kit::core::net {

std::optional<Endpoint> parse_ipv4(std::string_view host, std::uint16_t port) {
  // inet_pton reads a C string: `host` up to its first NUL, as a
  // std::string's c_str() gives it. A dotted quad has at most 15 characters,
  // so a longer one is none, and the copy needs no allocation.
  host = host.substr(0, host.find('\0'));
  char text[INET_ADDRSTRLEN] = {};
  if (host.size() >= sizeof text) return std::nullopt;
  // A part with a leading zero is none, as glibc's inet_pton reads it.
  // Apple's reads "010" as 10, and inet_aton as octal 8.
  for (std::size_t i = 0; i + 1 < host.size(); ++i) {
    if (host[i] == '0' && (i == 0 || host[i - 1] == '.') &&
        host[i + 1] >= '0' && host[i + 1] <= '9') {
      return std::nullopt;
    }
  }
  host.copy(text, host.size());
  in_addr address{};
  if (::inet_pton(AF_INET, text, &address) != 1) return std::nullopt;
  return Endpoint{ntohl(address.s_addr), port};
}

Result<Endpoint> parse_endpoint(std::string_view host, std::uint16_t port) try {
  if (host.empty()) return Status::invalid_argument("the host is empty");
  const std::optional<Endpoint> endpoint = parse_ipv4(host, port);
  if (!endpoint) {
    return Status::invalid_argument("\"" + std::string(host) +
                                    "\" is not a numeric IPv4 address");
  }
  if (endpoint->ipv4 == INADDR_ANY) {
    return Status::invalid_argument(
        "0.0.0.0 is the wildcard, not one host's address");
  }
  return *endpoint;
} catch (...) {
  return exception_status("parse_endpoint", Status::Code::IoError);
}

std::string to_string(const Endpoint& endpoint) {
  const sockaddr_in in = detail::to_sockaddr(endpoint);
  char text[INET_ADDRSTRLEN] = {};
  ::inet_ntop(AF_INET, &in.sin_addr, text, sizeof text);
  return std::string(text) + ":" + std::to_string(endpoint.port);
}

Result<std::vector<Endpoint>> resolve(std::string_view api,
                                      const std::string& host,
                                      std::uint16_t port) try {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* found = nullptr;
  const int error =
      ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found);
  const int system_error = errno;  // before the message's allocations
  const std::unique_ptr<addrinfo, void (*)(addrinfo*)> owned(found,
                                                             ::freeaddrinfo);
  if (error != 0) {
    const std::string why =
        std::string(api) + ": cannot look up the name: " +
        (error == EAI_SYSTEM ? std::generic_category().message(system_error)
                             : std::string(::gai_strerror(error)));
    if (error == EAI_NONAME) return Status::not_found(why);
    if (error == EAI_MEMORY) return Status::out_of_memory(why);
    return Status::io_error(why);
  }
  std::vector<Endpoint> addresses;
  for (const addrinfo* a = found; a != nullptr; a = a->ai_next) {
    if (a->ai_family != AF_INET || a->ai_addrlen != sizeof(sockaddr_in)) {
      continue;
    }
    sockaddr_in in{};
    std::memcpy(&in, a->ai_addr, sizeof in);
    addresses.push_back(detail::to_endpoint(in));
  }
  if (addresses.empty()) {
    return Status::not_found(
        std::string(api) +
        ": cannot look up the name: no IPv4 address for TCP");
  }
  return addresses;
} catch (...) {
  return exception_status(api, Status::Code::IoError);
}

Result<std::uint16_t> parse_port(std::string_view text) try {
  // Five digits at most, the first not 0: no sign, space, 0 or leading zero
  // passes, and the sum cannot overflow.
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (!text.empty() && text.size() <= 5 && text.front() != '0' &&
      std::all_of(text.begin(), text.end(), digit)) {
    std::uint32_t port = 0;
    for (const char c : text) {
      port = (port * 10) + static_cast<std::uint32_t>(c - '0');
    }
    if (port <= 65535) return static_cast<std::uint16_t>(port);
  }
  return Status::invalid_argument(
      "\"" + std::string(text) +
      "\" is not a port: a number from 1 to 65535, with no leading zero");
} catch (...) {
  return exception_status("parse_port", Status::Code::IoError);
}

Result<HostPort> parse_host_port(std::string_view text,
                                 std::uint16_t default_port) try {
  const std::string quoted = "\"" + std::string(text) + "\"";
  const std::size_t colon = text.find(':');
  // Before the split at the colon, which would read "[::1]:7101" as a bad
  // port and "::1" as no host.
  if ((!text.empty() && text.front() == '[') ||
      (colon != std::string_view::npos &&
       text.find(':', colon + 1) != std::string_view::npos)) {
    return Status::invalid_argument(quoted +
                                    " is IPv6, which is not supported");
  }
  HostPort at{std::string(text.substr(0, colon)), default_port};
  if (at.host.empty()) {
    return Status::invalid_argument(quoted + " names no host");
  }
  if (colon == std::string_view::npos) {
    if (default_port == 0) {
      return Status::invalid_argument(quoted + " names no port");
    }
    return at;
  }
  VKC_ASSIGN(at.port, parse_port(text.substr(colon + 1)));
  return at;
} catch (...) {
  return exception_status("parse_host_port", Status::Code::IoError);
}

}  // namespace volumetric_kit::core::net
