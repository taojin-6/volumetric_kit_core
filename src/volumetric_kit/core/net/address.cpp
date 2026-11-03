// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/net/address.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
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

}  // namespace volumetric_kit::core::net
