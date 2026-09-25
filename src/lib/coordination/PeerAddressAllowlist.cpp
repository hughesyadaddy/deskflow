/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/PeerAddressAllowlist.h"

#include "base/Log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cstring>
#include <utility>

namespace deskflow::coordination {

namespace {

std::string printAddress(const sockaddr *address)
{
  char buffer[INET6_ADDRSTRLEN] = {};
  if (address->sa_family == AF_INET) {
    const auto *v4 = reinterpret_cast<const sockaddr_in *>(address);
    if (inet_ntop(AF_INET, const_cast<in_addr *>(&v4->sin_addr), buffer, sizeof(buffer)) != nullptr) {
      return buffer;
    }
  } else if (address->sa_family == AF_INET6) {
    const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(address);
    if (inet_ntop(AF_INET6, const_cast<in6_addr *>(&v6->sin6_addr), buffer, sizeof(buffer)) != nullptr) {
      return buffer;
    }
  }
  return {};
}

} // namespace

std::string PeerAddressAllowlist::normalize(const std::string &address)
{
  std::string text = address;
  // "[fe80::1%en0]" / "fe80::1%en0" → "fe80::1"
  if (!text.empty() && text.front() == '[') {
    text.erase(0, 1);
    if (const auto close = text.find(']'); close != std::string::npos) {
      text.erase(close);
    }
  }
  if (const auto zone = text.find('%'); zone != std::string::npos) {
    text.erase(zone);
  }
  in_addr v4{};
  if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
    char buffer[INET_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET, &v4, buffer, sizeof(buffer)) != nullptr ? std::string(buffer) : std::string();
  }
  in6_addr v6{};
  if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
    // IPv4-mapped (::ffff:a.b.c.d): a dual-stack listener reports IPv4
    // peers this way; the configured literal is the plain IPv4.
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&v6);
    static const unsigned char kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(bytes, kMappedPrefix, sizeof(kMappedPrefix)) == 0) {
      in_addr mapped{};
      std::memcpy(&mapped, bytes + 12, sizeof(mapped));
      char buffer[INET_ADDRSTRLEN] = {};
      return inet_ntop(AF_INET, &mapped, buffer, sizeof(buffer)) != nullptr ? std::string(buffer) : std::string();
    }
    char buffer[INET6_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET6, &v6, buffer, sizeof(buffer)) != nullptr ? std::string(buffer) : std::string();
  }
  return {};
}

std::vector<std::string> PeerAddressAllowlist::systemResolve(const std::string &host)
{
  std::vector<std::string> result;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *list = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || list == nullptr) {
    return result;
  }
  for (const addrinfo *entry = list; entry != nullptr; entry = entry->ai_next) {
    const auto text = printAddress(entry->ai_addr);
    if (!text.empty()) {
      result.push_back(text);
    }
  }
  freeaddrinfo(list);
  return result;
}

PeerAddressAllowlist::PeerAddressAllowlist(std::vector<std::string> hosts, Resolver resolver)
    : m_resolver(std::move(resolver))
{
  if (!m_resolver) {
    m_resolver = &PeerAddressAllowlist::systemResolve;
  }
  for (auto &host : hosts) {
    if (host.empty()) {
      continue;
    }
    if (const auto literal = normalize(host); !literal.empty()) {
      m_literals.insert(literal);
      continue;
    }
    if (std::find(m_hosts.begin(), m_hosts.end(), host) == m_hosts.end()) {
      m_hosts.push_back(std::move(host));
    }
  }
}

PeerAddressAllowlist::~PeerAddressAllowlist()
{
  stop();
}

void PeerAddressAllowlist::start(double now)
{
  refreshIfDue(now);
}

void PeerAddressAllowlist::stop()
{
  std::thread worker;
  {
    std::scoped_lock lock{m_mutex};
    worker = std::move(m_thread);
  }
  if (worker.joinable()) {
    worker.join();
  }
}

bool PeerAddressAllowlist::allows(const std::string &address) const
{
  const auto normalized = normalize(address);
  if (normalized.empty()) {
    return false;
  }
  if (m_literals.count(normalized) != 0) {
    return true;
  }
  std::scoped_lock lock{m_mutex};
  return m_resolved.count(normalized) != 0;
}

void PeerAddressAllowlist::noteMiss(double now)
{
  std::scoped_lock lock{m_mutex};
  if (m_hosts.empty() || now - m_lastRefreshStartedAt < kMissRefreshMinGapS) {
    return;
  }
  m_refreshRequested = true;
}

void PeerAddressAllowlist::reapThreadLocked()
{
  // A finished resolve leaves a joinable thread behind; collect it before
  // starting the next one (never blocks: the thread has already exited).
  if (!m_resolving && m_thread.joinable()) {
    m_thread.join();
  }
}

void PeerAddressAllowlist::refreshIfDue(double now)
{
  std::scoped_lock lock{m_mutex};
  if (m_hosts.empty() || m_resolving) {
    return;
  }
  reapThreadLocked();
  const bool due = m_refreshRequested || now - m_lastRefreshStartedAt >= kRefreshS;
  if (!due) {
    return;
  }
  m_refreshRequested = false;
  m_lastRefreshStartedAt = now;
  m_resolving = true;
  m_thread = std::thread([this] { resolveAll(); });
}

void PeerAddressAllowlist::refreshNow(double now)
{
  {
    std::scoped_lock lock{m_mutex};
    if (m_resolving) {
      return; // the background pass will land shortly
    }
    reapThreadLocked();
    m_refreshRequested = false;
    m_lastRefreshStartedAt = now;
    m_resolving = true;
  }
  resolveAll();
}

void PeerAddressAllowlist::resolveAll()
{
  std::set<std::string> resolved;
  for (const auto &host : m_hosts) {
    for (const auto &address : m_resolver(host)) {
      if (const auto normalized = normalize(address); !normalized.empty()) {
        resolved.insert(normalized);
      }
    }
  }
  std::scoped_lock lock{m_mutex};
  if (resolved.empty() && !m_resolved.empty()) {
    // DNS blip: keep the last good answer rather than locking every
    // peer out until the next refresh.
    LOG_DEBUG("coordination: peer address refresh returned nothing; keeping the previous set");
  } else {
    m_resolved = std::move(resolved);
  }
  m_resolving = false;
}

std::vector<std::string> PeerAddressAllowlist::hosts() const
{
  return m_hosts;
}

std::vector<std::string> PeerAddressAllowlist::snapshot() const
{
  std::vector<std::string> result(m_literals.begin(), m_literals.end());
  std::scoped_lock lock{m_mutex};
  result.insert(result.end(), m_resolved.begin(), m_resolved.end());
  return result;
}

} // namespace deskflow::coordination
