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
the lines are stale, and every periodic sender re-posts.

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

  PeerOutbox(std::string ip, std::string lan, Transport transport, Clock clock);
  PeerOutbox(const PeerOutbox &) = delete;
  PeerOutbox &operator=(const PeerOutbox &) = delete;
  ~PeerOutbox();

  void start();
  void stop();

  //! Queue \p line for delivery (kept across backoff; the oldest line is
  //! dropped past kMaxQueuedLines). \p onReply runs on the lane thread with
  //! the peer's reply line when set (query semantics); never on failure.
  void post(std::string line, ReplyHandler onReply = {});

  //! Queue \p line for a key forward; never blocks past \p graceMs.
  /*!
  Returns whether the caller may treat the line as delivered: true when the
  peer is reachable (the send is in flight), false in backoff. Inside the
  backoff window nothing is queued; once the window has opened the line is
  queued anyway (still reported as not delivered) so the attempt re-settles
  the state -- a client posts nothing else on the server lane between
  version probes, and without this a single transient failure kept keys
  local until the next probe. While reachability is still Unknown the call
  waits at most \p graceMs for the attempt to resolve; on timeout the line
  is withdrawn from the queue when it has not been picked up yet, so a key
  reported as "kept local" is never also delivered late.
  */
  bool forward(std::string line, int graceMs);

  State state() const;
  //! Address tried first on the next attempt (last one that answered).
  std::string preferredAddress() const;
  //! Monotonic time before which no connect attempt is made (0 = now).
  double nextAttemptAt() const;
  //! No queued lines and no attempt in flight.
  bool idle() const;

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
  };

  void run();
  //! Append a job; drops (and logs) the oldest past kMaxQueuedLines.
  uint64_t enqueueLocked(std::string line, ReplyHandler onReply);
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
  //! Jobs posted / resolved (sent, failed, or dropped), in FIFO order, so
  //! forward() can wait for its own job.
  uint64_t m_posted = 0;
  uint64_t m_resolved = 0;
  bool m_stop = false;
  std::thread m_thread;
};

} // namespace deskflow::coordination
