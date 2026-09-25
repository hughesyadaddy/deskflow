/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace deskflow::coordination {

//! Every address a configured peer name resolves to (IPv4 + IPv6).
/*!
The mesh listens on every interface and, without a shared token, accepts
any line from anyone; fleet-wide commands (`rescue`, `stopall`) are
therefore accepted only from a SOURCE ADDRESS that belongs to a configured
peer: each peer's `ip` and `lan` entries (LAN name, Tailscale FQDN, or a
literal). Literals are allowed immediately; names are resolved on a
background thread at start, refreshed every kRefreshS, and re-resolved
early (rate-limited) after a miss -- the mesh reader thread never blocks
on DNS. A shared `token` remains the stronger option: when configured it
is still required (the transport drops bad tokens before this check).
*/
class PeerAddressAllowlist
{
public:
  //! Numeric addresses a host name resolves to (empty on failure).
  using Resolver = std::function<std::vector<std::string>(const std::string &host)>;

  static constexpr double kRefreshS = 300.0;
  //! Minimum spacing between miss-triggered re-resolves.
  static constexpr double kMissRefreshMinGapS = 5.0;

  //! \p hosts: every peer address entry (names and literals, duplicates
  //! fine). \p resolver defaults to getaddrinfo (systemResolve).
  explicit PeerAddressAllowlist(std::vector<std::string> hosts, Resolver resolver = {});
  PeerAddressAllowlist(const PeerAddressAllowlist &) = delete;
  PeerAddressAllowlist &operator=(const PeerAddressAllowlist &) = delete;
  ~PeerAddressAllowlist();

  //! Kick off the first background resolve (\p now = monotonic seconds).
  void start(double now);
  //! Join the resolver thread.
  void stop();

  //! True when \p address (numeric, any family) belongs to a peer.
  bool allows(const std::string &address) const;

  //! A command from an unknown address was dropped: schedule an early
  //! refresh (a peer's address may have changed) without blocking.
  void noteMiss(double now);

  //! Worker tick: start a background resolve when one is due.
  void refreshIfDue(double now);

  //! Resolve synchronously on the calling thread (tests, start-up).
  void refreshNow(double now);

  //! Names still to resolve (literals excluded).
  std::vector<std::string> hosts() const;
  //! Current allowed addresses (normalized).
  std::vector<std::string> snapshot() const;

  //! Canonical text form: IPv4-mapped IPv6 → IPv4, zone/brackets stripped,
  //! parseable literals re-printed. Empty when \p address is not numeric.
  static std::string normalize(const std::string &address);
  //! getaddrinfo(host) → numeric addresses (both families).
  static std::vector<std::string> systemResolve(const std::string &host);

private:
  void resolveAll();
  void reapThreadLocked();

  Resolver m_resolver;
  std::vector<std::string> m_hosts;   //!< names needing resolution
  std::set<std::string> m_literals;   //!< always allowed
  mutable std::mutex m_mutex;
  std::set<std::string> m_resolved;   //!< guarded by m_mutex
  bool m_resolving = false;           //!< guarded by m_mutex
  bool m_refreshRequested = false;    //!< guarded by m_mutex
  double m_lastRefreshStartedAt = -1.0e9; //!< guarded by m_mutex
  std::thread m_thread;               //!< guarded by m_mutex (join/assign)
};

} // namespace deskflow::coordination
