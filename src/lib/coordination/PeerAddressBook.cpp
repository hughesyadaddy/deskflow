/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/PeerAddressBook.h"

#include "base/Log.h"
#include "common/FleetCursor.h"

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
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

namespace deskflow::coordination {

using deskflow::common::namesEqual;

namespace {

void appendUnique(std::vector<std::string> &out, const std::string &address)
{
  if (address.empty() || std::find(out.begin(), out.end(), address) != out.end()) {
    return;
  }
  if (out.size() < PeerAddressBook::kMaxCandidates) {
    out.push_back(address);
  }
}

void eraseValue(std::deque<std::string> &values, const std::string &value)
{
  values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

void eraseValue(std::vector<std::string> &values, const std::string &value)
{
  values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

} // namespace

bool PeerAddressBook::parseIPv4(const std::string &address, std::string *canonical)
{
  in_addr v4{};
  if (address.empty() || inet_pton(AF_INET, address.c_str(), &v4) != 1) {
    return false;
  }
  if (canonical != nullptr) {
    char buffer[INET_ADDRSTRLEN] = {};
    *canonical = inet_ntop(AF_INET, &v4, buffer, sizeof(buffer)) != nullptr ? std::string(buffer) : std::string();
    return !canonical->empty();
  }
  return true;
}

bool PeerAddressBook::usableAddress(const std::string &address, std::string *canonical)
{
  in_addr v4{};
  if (address.empty() || inet_pton(AF_INET, address.c_str(), &v4) != 1) {
    return false;
  }
  const uint32_t host = ntohl(v4.s_addr);
  const bool unspecified = host == 0;
  const bool loopback = (host >> 24) == 127;
  const bool linkLocal = (host >> 16) == 0xa9fe; // 169.254/16
  const bool multicast = (host >> 28) == 0xe; // 224/4
  const bool broadcast = host == 0xffffffffU;
  if (unspecified || loopback || linkLocal || multicast || broadcast) {
    return false;
  }
  if (canonical != nullptr) {
    char buffer[INET_ADDRSTRLEN] = {};
    *canonical = inet_ntop(AF_INET, &v4, buffer, sizeof(buffer)) != nullptr ? std::string(buffer) : std::string();
    if (canonical->empty()) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> PeerAddressBook::systemResolve(const std::string &host)
{
  std::vector<std::string> result;
  addrinfo hints{};
  hints.ai_family = AF_INET; // the mesh listener is IPv4-only
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *list = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || list == nullptr) {
    return result;
  }
  for (const addrinfo *entry = list; entry != nullptr; entry = entry->ai_next) {
    if (entry->ai_family != AF_INET) {
      continue;
    }
    const auto *v4 = reinterpret_cast<const sockaddr_in *>(entry->ai_addr);
    char buffer[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, const_cast<in_addr *>(&v4->sin_addr), buffer, sizeof(buffer)) != nullptr) {
      result.emplace_back(buffer);
    }
  }
  freeaddrinfo(list);
  return result;
}

PeerAddressBook::PeerAddressBook(const PeerList &peers, Resolver resolver) : m_shared(std::make_shared<Shared>())
{
  m_shared->resolver = resolver ? std::move(resolver) : &PeerAddressBook::systemResolve;
  for (const auto &peer : peers) {
    if (peer.name.empty() || m_shared->entries.contains(peer.name)) {
      continue;
    }
    Entry entry;
    entry.name = peer.name;
    // A slot is either a usable literal or a name to resolve; a literal
    // that can never be connected to (loopback, 0.0.0.0, ...) is dropped
    // here so it never costs a connect timeout.
    const auto classify = [](const std::string &slot, std::string &literal, std::string &name) {
      if (slot.empty()) {
        return;
      }
      // A configured literal is the operator's word: loopback and
      // link-local are accepted here (tests and single-host setups point
      // at 127.0.0.1); only addresses nothing can listen on are dropped.
      // Resolved and learned addresses go through the strict usableAddress().
      std::string canonical;
      if (parseIPv4(slot, &canonical)) {
        if (canonical != "0.0.0.0" && canonical != "255.255.255.255" && !usableAddress(canonical) &&
            (canonical.rfind("224.", 0) == 0 || canonical.rfind("239.", 0) == 0)) {
          return; // multicast
        }
        if (canonical != "0.0.0.0" && canonical != "255.255.255.255") {
          literal = canonical;
        }
        return;
      }
      name = slot;
    };
    classify(peer.lan, entry.lanLiteral, entry.lanName);
    classify(peer.ip, entry.ipLiteral, entry.ipName);
    m_shared->entries.emplace(peer.name, std::move(entry));
  }
}

PeerAddressBook::~PeerAddressBook()
{
  stop();
}

void PeerAddressBook::setChangedHandler(ChangedHandler handler)
{
  std::scoped_lock lock{m_shared->mutex};
  m_shared->changed = std::move(handler);
}

void PeerAddressBook::start(double now)
{
  refreshIfDue(now);
}

void PeerAddressBook::stop()
{
  std::scoped_lock lock{m_shared->mutex};
  m_shared->stopped = true;
  m_shared->changed = nullptr;
}

PeerAddressBook::Entry *PeerAddressBook::entryLocked(Shared &shared, const std::string &peerName)
{
  for (auto &[name, entry] : shared.entries) {
    if (namesEqual(name, peerName)) {
      return &entry;
    }
  }
  return nullptr;
}

const PeerAddressBook::Entry *PeerAddressBook::entryLocked(const Shared &shared, const std::string &peerName)
{
  for (const auto &[name, entry] : shared.entries) {
    if (namesEqual(name, peerName)) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<std::string> PeerAddressBook::candidatesLocked(const Shared &shared, const Entry &entry)
{
  std::vector<std::string> out;
  appendUnique(out, entry.lastAnswered);
  const auto appendTier = [&](const std::string &literal, const std::string &name) {
    if (!literal.empty()) {
      appendUnique(out, literal);
      return;
    }
    if (name.empty()) {
      return;
    }
    if (const auto found = shared.resolved.find(name); found != shared.resolved.end()) {
      for (const auto &address : found->second) {
        appendUnique(out, address);
      }
    }
  };
  appendTier(entry.lanLiteral, entry.lanName);
  appendTier(entry.ipLiteral, entry.ipName);
  for (const auto &address : entry.learned) {
    appendUnique(out, address);
  }
  return out;
}

std::vector<std::string> PeerAddressBook::candidates(const std::string &peerName) const
{
  std::scoped_lock lock{m_shared->mutex};
  const auto *entry = entryLocked(*m_shared, peerName);
  return entry == nullptr ? std::vector<std::string>{} : candidatesLocked(*m_shared, *entry);
}

std::string PeerAddressBook::lanCandidate(const std::string &peerName) const
{
  std::scoped_lock lock{m_shared->mutex};
  const auto *entry = entryLocked(*m_shared, peerName);
  if (entry == nullptr) {
    return {};
  }
  if (!entry->lanLiteral.empty()) {
    return entry->lanLiteral;
  }
  if (const auto found = m_shared->resolved.find(entry->lanName);
      !entry->lanName.empty() && found != m_shared->resolved.end() && !found->second.empty()) {
    return found->second.front();
  }
  return {};
}

void PeerAddressBook::noteAnswered(const std::string &peerName, const std::string &address)
{
  std::string canonical;
  if (!parseIPv4(address, &canonical)) {
    return;
  }
  std::scoped_lock lock{m_shared->mutex};
  auto *entry = entryLocked(*m_shared, peerName);
  if (entry == nullptr || entry->lastAnswered == canonical) {
    return;
  }
  // Only an address this lane could have been handed answers: anything
  // else is a caller bug, never a new candidate.
  const auto current = candidatesLocked(*m_shared, *entry);
  if (std::find(current.begin(), current.end(), canonical) == current.end()) {
    return;
  }
  entry->lastAnswered = canonical;
  if (m_shared->changed && !m_shared->stopped) {
    m_shared->changed(entry->name);
  }
}

void PeerAddressBook::learn(const std::string &peerName, const std::string &address)
{
  std::string canonical;
  if (!usableAddress(address, &canonical)) {
    return;
  }
  std::scoped_lock lock{m_shared->mutex};
  auto *entry = entryLocked(*m_shared, peerName);
  if (entry == nullptr) {
    return;
  }
  // Already a candidate (configured, resolved, answered or learned): nothing
  // to add, and in particular no reordering -- only an answer moves an
  // address forward.
  const auto current = candidatesLocked(*m_shared, *entry);
  if (std::find(current.begin(), current.end(), canonical) != current.end()) {
    return;
  }
  entry->learned.push_front(canonical);
  while (entry->learned.size() > kMaxLearned) {
    entry->learned.pop_back();
  }
  LOG_INFO("coordination: learned %s as a candidate address for peer \"%s\"", canonical.c_str(), entry->name.c_str());
  if (m_shared->changed && !m_shared->stopped) {
    m_shared->changed(entry->name);
  }
}

void PeerAddressBook::evict(const std::string &peerName, const std::string &address)
{
  std::string canonical;
  if (!parseIPv4(address, &canonical)) {
    return;
  }
  std::scoped_lock lock{m_shared->mutex};
  auto *entry = entryLocked(*m_shared, peerName);
  if (entry == nullptr) {
    return;
  }
  bool changed = false;
  if (entry->lastAnswered == canonical) {
    entry->lastAnswered.clear();
    changed = true;
  }
  if (std::find(entry->learned.begin(), entry->learned.end(), canonical) != entry->learned.end()) {
    eraseValue(entry->learned, canonical);
    changed = true;
  }
  for (const auto *name : {&entry->lanName, &entry->ipName}) {
    if (name->empty()) {
      continue;
    }
    if (auto found = m_shared->resolved.find(*name); found != m_shared->resolved.end()) {
      const auto before = found->second.size();
      eraseValue(found->second, canonical);
      changed = changed || found->second.size() != before;
    }
  }
  // A configured literal is the operator's word; it is never evicted, only
  // reported.
  if (entry->lanLiteral == canonical || entry->ipLiteral == canonical) {
    LOG_WARN(
        "coordination: configured address %s for peer \"%s\" answered as another seat -- check coordination/peers",
        canonical.c_str(), entry->name.c_str()
    );
  }
  if (changed) {
    LOG_INFO("coordination: evicted %s as a candidate address for peer \"%s\"", canonical.c_str(), entry->name.c_str());
    if (m_shared->changed && !m_shared->stopped) {
      m_shared->changed(entry->name);
    }
    m_shared->refreshRequested = true; // the name may have moved: re-resolve early
  }
}

void PeerAddressBook::noteMiss(double now)
{
  std::scoped_lock lock{m_shared->mutex};
  if (now - m_shared->lastRefreshStartedAt < kMissRefreshMinGapS) {
    return;
  }
  m_shared->refreshRequested = true;
}

std::vector<std::string> PeerAddressBook::names() const
{
  std::scoped_lock lock{m_shared->mutex};
  std::vector<std::string> out;
  for (const auto &[peerName, entry] : m_shared->entries) {
    for (const auto *name : {&entry.lanName, &entry.ipName}) {
      if (!name->empty() && std::find(out.begin(), out.end(), *name) == out.end()) {
        out.push_back(*name);
      }
    }
  }
  return out;
}

void PeerAddressBook::storeResolvedLocked(Shared &shared, const std::string &host, std::vector<std::string> addresses)
{
  std::vector<std::string> usable;
  for (const auto &address : addresses) {
    std::string canonical;
    if (usableAddress(address, &canonical) && std::find(usable.begin(), usable.end(), canonical) == usable.end()) {
      usable.push_back(canonical);
    }
    if (usable.size() >= kMaxResolvedPerName) {
      break;
    }
  }
  auto &slot = shared.resolved[host];
  // A failed or empty resolve keeps the previous answers: a transient DNS
  // blip must not strip a lane of addresses that worked a minute ago. A
  // wrong address is removed by evict() (answered as another seat) or
  // replaced by the next successful resolve.
  if (usable.empty()) {
    if (slot.empty()) {
      LOG_DEBUG("coordination: %s did not resolve to a usable IPv4 address", host.c_str());
    }
    return;
  }
  if (slot == usable) {
    return;
  }
  slot = std::move(usable);
  std::string joined;
  for (const auto &address : slot) {
    joined += (joined.empty() ? "" : ", ") + address;
  }
  LOG_INFO("coordination: %s resolves to %s", host.c_str(), joined.c_str());
  if (shared.changed && !shared.stopped) {
    for (const auto &[peerName, entry] : shared.entries) {
      if (entry.lanName == host || entry.ipName == host) {
        shared.changed(entry.name);
      }
    }
  }
}

void PeerAddressBook::resolveOne(const std::shared_ptr<Shared> &shared, const std::string &host)
{
  Resolver resolver;
  {
    std::scoped_lock lock{shared->mutex};
    resolver = shared->resolver;
  }
  std::vector<std::string> addresses = resolver ? resolver(host) : std::vector<std::string>{};
  std::scoped_lock lock{shared->mutex};
  shared->inFlight.erase(host);
  if (shared->stopped) {
    return; // the book is gone from the lanes' point of view
  }
  storeResolvedLocked(*shared, host, std::move(addresses));
}

void PeerAddressBook::refreshIfDue(double now)
{
  std::vector<std::string> toResolve;
  {
    std::scoped_lock lock{m_shared->mutex};
    if (m_shared->stopped) {
      return;
    }
    const bool due = m_shared->refreshRequested || now - m_shared->lastRefreshStartedAt >= kRefreshS;
    if (!due) {
      return;
    }
    m_shared->refreshRequested = false;
    m_shared->lastRefreshStartedAt = now;
    for (const auto &[peerName, entry] : m_shared->entries) {
      for (const auto *name : {&entry.lanName, &entry.ipName}) {
        if (name->empty() || m_shared->inFlight.contains(*name)) {
          continue; // one resolve per name at a time; a 35 s mDNS stall must not pile up threads
        }
        if (std::find(toResolve.begin(), toResolve.end(), *name) == toResolve.end()) {
          toResolve.push_back(*name);
          m_shared->inFlight.insert(*name);
        }
      }
    }
  }
  for (const auto &host : toResolve) {
    // Detached on purpose: the thread owns only a shared_ptr to the state,
    // so stop()/destruction never waits on an unbounded getaddrinfo.
    std::thread([shared = m_shared, host] { resolveOne(shared, host); }).detach();
  }
}

void PeerAddressBook::refreshNow(double now)
{
  std::vector<std::string> toResolve;
  {
    std::scoped_lock lock{m_shared->mutex};
    if (m_shared->stopped) {
      return;
    }
    m_shared->refreshRequested = false;
    m_shared->lastRefreshStartedAt = now;
    for (const auto &[peerName, entry] : m_shared->entries) {
      for (const auto *name : {&entry.lanName, &entry.ipName}) {
        if (!name->empty() && !m_shared->inFlight.contains(*name) &&
            std::find(toResolve.begin(), toResolve.end(), *name) == toResolve.end()) {
          toResolve.push_back(*name);
          m_shared->inFlight.insert(*name);
        }
      }
    }
  }
  for (const auto &host : toResolve) {
    resolveOne(m_shared, host);
  }
}

} // namespace deskflow::coordination
