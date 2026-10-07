/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/Peer.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace deskflow::coordination {

//! Numeric connect candidates per configured peer, kept off the lane thread.
/*!
A PeerOutbox used to hand the peer's configured `ip`/`lan` STRINGS to the
transport, which ran `getaddrinfo` (unbounded) plus a bounded connect per
line, on the lane thread. With an mDNS name that takes 25-35 s to resolve
(or never does), every relayed key stalled the lane: a Down whose Up was
withdrawn at the forward grace stayed held on the peer, and the failure
resync only landed after the backoff window. The lane now connects to
NUMERIC addresses only, and this book is where they come from:

- configured literals are candidates immediately;
- configured names are resolved on detached background threads (at start,
  every kRefreshS, early after a miss) and contribute at most
  kMaxResolvedPerName IPv4 results each (the mesh listener is IPv4-only);
- the address that most recently ANSWERED is tried first (noteAnswered);
- a token-authenticated peer's observed source address can be learned
  (learn(); the caller enforces the token gate -- see behavior-spec) and
  sits at the END of the list until a send to it succeeds;
- loopback, unspecified, link-local, multicast and broadcast addresses are
  never candidates, whatever their origin.

Order: last-answered, then the `lan` tier (literal or its resolutions),
then the `ip` tier, then learned addresses; duplicates removed; at most
kMaxCandidates in all, so a lane walks a bounded number of 700 ms
connects. A changed handler wakes the lane the moment a name resolves so
a cold start never waits out a backoff window for an address it now has.

Threading: every public method is safe from any thread. Resolver threads
hold a shared_ptr to the book's state, never `this`, so stop() returns
immediately even while a 35 s getaddrinfo is still in flight; a result
landing after stop() is discarded. The changed handler is invoked with
the book's lock held, so stop() (which clears it) cannot race a lane
teardown; the handler must therefore only call into a PeerOutbox (whose
lock is never held while calling the book).
*/
class PeerAddressBook
{
public:
  //! `host` -> numeric IPv4 addresses (empty on failure). Blocking.
  using Resolver = std::function<std::vector<std::string>(const std::string &host)>;
  //! Candidates for \p peerName changed (resolution landed, answer noted,
  //! address learned or evicted). Invoked with the book's lock held.
  using ChangedHandler = std::function<void(const std::string &peerName)>;

  static constexpr double kRefreshS = 300.0;
  //! Minimum spacing between miss-triggered early refreshes.
  static constexpr double kMissRefreshMinGapS = 5.0;
  static constexpr size_t kMaxResolvedPerName = 2;
  static constexpr size_t kMaxLearned = 2;
  static constexpr size_t kMaxCandidates = 6;

  //! \p resolver defaults to getaddrinfo (systemResolve, IPv4 only).
  explicit PeerAddressBook(const PeerList &peers, Resolver resolver = {});
  PeerAddressBook(const PeerAddressBook &) = delete;
  PeerAddressBook &operator=(const PeerAddressBook &) = delete;
  ~PeerAddressBook();

  void setChangedHandler(ChangedHandler handler);

  //! Kick off the first background resolve (\p now = monotonic seconds).
  void start(double now);
  //! Stop handing out changes; in-flight resolves are abandoned (never joined).
  void stop();

  //! Numeric addresses to try for \p peerName, in connect order (see class
  //! comment). Empty while nothing has resolved yet.
  std::vector<std::string> candidates(const std::string &peerName) const;
  //! The `lan`-tier address tried first for \p peerName (LAN-first probes).
  std::string lanCandidate(const std::string &peerName) const;

  //! A send to \p address succeeded: it is tried first from now on.
  void noteAnswered(const std::string &peerName, const std::string &address);
  //! \p address was observed as the source of a message that the caller
  //! has authenticated as coming from \p peerName. Appended (kMaxLearned,
  //! newest kept); never moved ahead of configured addresses until it
  //! answers. Unusable addresses are ignored.
  void learn(const std::string &peerName, const std::string &address);
  //! \p address answered as a different seat: drop it from every tier.
  void evict(const std::string &peerName, const std::string &address);

  //! A command from an unknown address was dropped / a lane failed:
  //! schedule an early refresh of the names (rate-limited).
  void noteMiss(double now);
  //! Worker tick: start a background resolve when one is due.
  void refreshIfDue(double now);
  //! Resolve synchronously on the calling thread (tests, start-up).
  void refreshNow(double now);

  //! Names still subject to resolution (literals excluded).
  std::vector<std::string> names() const;

  //! True for a numeric IPv4 address that a peer could actually be
  //! listening on (not loopback, unspecified, link-local, multicast or
  //! broadcast; 240/4 is allowed -- tests use it as a black hole). The canonical text form is returned through \p
  //! canonical.
  static bool usableAddress(const std::string &address, std::string *canonical = nullptr);
  //! Numeric IPv4 of any kind (canonical text through \p canonical).
  static bool parseIPv4(const std::string &address, std::string *canonical = nullptr);
  //! getaddrinfo(host, AF_INET) -> numeric addresses. Blocking, unbounded.
  static std::vector<std::string> systemResolve(const std::string &host);

private:
  struct Entry
  {
    std::string name;
    std::string lanLiteral; //!< canonical numeric, or empty
    std::string ipLiteral;
    std::string lanName; //!< needs resolution, or empty
    std::string ipName;
    std::string lastAnswered;
    std::deque<std::string> learned; //!< newest first
  };
  struct Shared
  {
    mutable std::mutex mutex;
    Resolver resolver;
    std::map<std::string, Entry> entries;                     //!< by configured name
    std::map<std::string, std::vector<std::string>> resolved; //!< name -> addresses
    std::set<std::string> inFlight;                           //!< names being resolved
    ChangedHandler changed;
    bool stopped = false;
    bool refreshRequested = false;
    double lastRefreshStartedAt = -1.0e9;
  };

  static void resolveOne(const std::shared_ptr<Shared> &shared, const std::string &host);
  static void storeResolvedLocked(Shared &shared, const std::string &host, std::vector<std::string> addresses);
  static std::vector<std::string> candidatesLocked(const Shared &shared, const Entry &entry);
  static Entry *entryLocked(Shared &shared, const std::string &peerName);
  static const Entry *entryLocked(const Shared &shared, const std::string &peerName);

  std::shared_ptr<Shared> m_shared;
};

} // namespace deskflow::coordination
