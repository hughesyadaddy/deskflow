/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "coordination/CoordinationProtocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace deskflow::coordination {

//! Coordination-mesh TCP transport.
/*!
Accepts inbound newline-JSON messages on the coordination port and sends
one-shot messages to peer addresses. Mirrors the legacy coordinator's
connection model (one TCP connect per send, no persistent mesh channel)
so it is wire-compatible with kvmctl.

Threading: a dedicated accept thread queues each connection for a small
fixed pool of handler threads (excess connections are refused), so one
slow or stalled peer can never head-of-line block the mesh and inbound
load can never spawn unbounded threads. sendTo()/query() are blocking
(connect timeout bounded) and are meant to run on a PeerOutbox thread,
never on the worker tick or an input hook. The receive callback is
invoked on a handler thread.
*/
class CoordinationMesh
{
public:
  //! ``reply`` writes a status response back on the same connection.
  using Receiver = std::function<void(const Message &message, const std::function<void(const std::string &)> &reply)>;

  //! Inbound handler threads; connections beyond the pool wait in a short
  //! queue and the rest are refused (see kMaxConcurrentClients).
  static constexpr int kHandlerPoolSize = 4;

  CoordinationMesh(int port, std::string token, Receiver receiver);
  CoordinationMesh(const CoordinationMesh &) = delete;
  CoordinationMesh &operator=(const CoordinationMesh &) = delete;
  ~CoordinationMesh();

  bool start();
  void stop();

  //! Bound listen port (resolves an ephemeral port 0 after start()).
  int port() const
  {
    return m_port;
  }

  //! Handler threads currently inside handleClient (never exceeds the pool).
  int busyHandlers() const
  {
    return m_activeClients.load();
  }

  //! Fire-and-forget send to ``host:port`` (connect timeout bounded).
  //! Returns false when the peer is unreachable (connect timeout).
  bool sendTo(const std::string &host, const std::string &line);

  //! Connect-probe a peer's coordination port (LAN-first reachability).
  bool probe(const std::string &host, int timeoutMs);

  //! Connect-probe the local deskflow transport port (server wedge check).
  bool probeDeskflowPort(int port, int timeoutMs);

  //! Send one line and read one reply line (status query); empty on failure.
  std::string query(const std::string &host, const std::string &line);

  //! True when ``token`` is unset, or the message carries the right token.
  bool tokenOk(const Message &message) const;

private:
  void serveLoop();
  void handlerLoop();
  void handleClient(int clientFd);

  int m_port;
  std::string m_token;
  Receiver m_receiver;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  int m_listenFd = -1;

  // Accepted connections wait in m_pendingFds for a pooled handler; every
  // open client fd stays in m_clientFds so stop() can shut them down and
  // join the pool (handlers must never outlive *this).
  std::mutex m_clientsMutex;
  std::condition_variable m_pendingReady;
  std::deque<int> m_pendingFds;
  std::set<int> m_clientFds;
  std::atomic<int> m_activeClients{0};
  std::vector<std::thread> m_handlers;
};

//! Outbound lane to one peer: callers enqueue and return immediately; a
//! dedicated thread performs the blocking connect+send.
/*!
Reachability is a small state machine. \c Unknown until the first attempt
resolves, \c Reachable while sends succeed, \c Backoff after a failure with
the retry delay doubling from 1 s to 30 s. In \c Backoff the lane makes one
connect attempt per window, alternating the LAN and stable addresses, so a
sleeping peer costs one connect timeout per window instead of two per
heartbeat. The last address that answered is tried first. Queued lines are
capped, and a failed attempt discards everything queued behind it: by then
the lines are stale, and every periodic sender re-posts. Two job classes
refine this: KEY lines (forward()) are never queued in backoff and expire
kKeyDeadlineS after enqueue, so a key is never delivered late; STICKY
lines (postSticky()) survive failed attempts, so a boundary resync always
reaches the peer once it answers again.

The transport and clock are injectable so the schedule is unit-testable
without sockets or a thread (tests call pump() directly).
*/
class PeerOutbox
{
public:
  enum class State
  {
    Unknown,
    Reachable,
    Backoff
  };

  using ReplyHandler = std::function<void(const std::string &reply)>;
  //! Blocking one-shot transport. When \p reply is non-null the transport
  //! must read one reply line into it; an empty reply counts as failure.
  using Transport = std::function<bool(const std::string &host, const std::string &line, std::string *reply)>;
  //! Monotonic seconds.
  using Clock = std::function<double()>;

  static constexpr double kBackoffMinS = 1.0;
  static constexpr double kBackoffMaxS = 30.0;
  //! Queue depth per lane. Deep enough that a burst of key events behind
  //! one connect never drops a Down while its Up still delivers (stuck
  //! modifier); every drop is logged.
  static constexpr size_t kMaxQueuedLines = 64;
  //! A key line still queued this long after forward() enqueued it is
  //! discarded before any connect: the hook has long since handled the key
  //! locally, and typing it late on the peer is the phantom-Down bug.
  static constexpr double kKeyDeadlineS = 0.150;

  //! Invoked (on the lane thread, no lock held) when an attempt fails while
  //! the lane was not already in backoff. Keys forwarded before the failure
  //! may be held on the peer with their Up now undeliverable; the handler
  //! resyncs (Coordinator: ledger release + sticky KeyClearAll).
  using FailureHandler = std::function<void()>;

  PeerOutbox(std::string ip, std::string lan, Transport transport, Clock clock);
  PeerOutbox(const PeerOutbox &) = delete;
  PeerOutbox &operator=(const PeerOutbox &) = delete;
  ~PeerOutbox();

  void start();
  void stop();

  void setFailureHandler(FailureHandler handler);

  //! Queue \p line for delivery (kept across backoff; the oldest line is
  //! dropped past kMaxQueuedLines). \p onReply runs on the lane thread with
  //! the peer's reply line when set (query semantics); never on failure.
  void post(std::string line, ReplyHandler onReply = {});

  //! Queue \p line so that it survives failed attempts: a failure keeps it
  //! at the head of the queue (everything else queued is dropped) and it
  //! goes out on the first attempt that succeeds. A second sticky post of
  //! the same line replaces the first. For resync lines (KeyClearAll).
  void postSticky(std::string line);

  //! Queue \p line for a key forward; never blocks past \p graceMs.
  /*!
  Returns whether the caller may treat the line as delivered. In backoff
  nothing is queued and the answer is false (the key stays local); a key is
  never both typed locally and delivered late. Otherwise the call waits at
  most \p graceMs for its own send to complete and reports the outcome; on
  timeout the line is withdrawn when it has not been picked up yet, and the
  answer is false. A key that sits queued past kKeyDeadlineS is discarded
  by pump() before any connect is attempted for it.
  */
  bool forward(std::string line, int graceMs);

  //! Drop every queued key line (rescue: the fleet is restarting anyway).
  void discardKeys();

  State state() const;
  //! Address tried first on the next attempt (last one that answered).
  std::string preferredAddress() const;
  //! Monotonic time before which no connect attempt is made (0 = now).
  double nextAttemptAt() const;
  //! No queued lines and no attempt in flight.
  bool idle() const;
  //! Key lines discarded past their deadline since construction.
  uint64_t expiredKeys() const;

  //! Drive one scheduling step at \p now: attempts the queued lines when the
  //! backoff window has opened. The lane thread calls this; tests call it
  //! directly with an injected clock.
  void pump(double now);

private:
  struct Job
  {
    std::string line;
    ReplyHandler onReply;
    uint64_t ticket = 0; //!< m_posted value at enqueue (forward() withdrawal)
    bool isKey = false;  //!< key class: never queued in backoff, deadline-bound
    bool sticky = false; //!< survives failed attempts (see postSticky)
    double deadline = 0; //!< monotonic seconds; 0 = none
  };

  void run();
  //! Append a job; drops (and logs) the oldest past kMaxQueuedLines.
  uint64_t enqueueLocked(Job job);
  //! True while the job with \p ticket is queued or in flight.
  bool pendingLocked(uint64_t ticket) const;
  std::string otherAddressLocked(const std::string &host) const;

  const std::string m_ip;
  const std::string m_lan;
  Transport m_transport;
  Clock m_clock;

  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::condition_variable m_jobDone;
  std::deque<Job> m_queue;
  State m_state = State::Unknown;
  bool m_preferLan = true;
  double m_backoffS = 0.0;
  double m_nextAttemptAt = 0.0;
  bool m_inFlight = false;
  //! Ticket counter; forward() waits until its own ticket is neither
  //! queued nor in flight, then collects the outcome from m_deliveredKeys.
  uint64_t m_posted = 0;
  uint64_t m_inFlightTicket = 0;
  std::set<uint64_t> m_deliveredKeys;
  uint64_t m_expiredKeys = 0;
  FailureHandler m_onFailure;
  bool m_stop = false;
  std::thread m_thread;
};

} // namespace deskflow::coordination
