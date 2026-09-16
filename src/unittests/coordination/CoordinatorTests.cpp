/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// Hotspot D1-coordinator-blocking-connects: peer I/O must never block the
// worker tick or an OS input callback. Covers the PeerOutbox backoff
// schedule (injected clock + transport), the non-blocking Coordinator
// paths against unreachable peers, and the bounded mesh handler pool.

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "coordination/CoordinationMesh.h"
#include "coordination/CoordinationProtocol.h"
#include "coordination/Coordinator.h"
#include "coordination/KeyboardRelayMonitor.h"
#include "coordination/Peer.h"
#include "deskflow/KeyTypes.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using deskflow::coordination::CoordinationMesh;
using deskflow::coordination::Coordinator;
using deskflow::coordination::CoordinatorConfig;
using deskflow::coordination::KeyForwardResult;
using deskflow::coordination::Message;
using deskflow::coordination::PeerOutbox;
namespace protocol = deskflow::coordination::protocol;

class CoordinatorTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void outbox_backoffDoublesToCapAndTriesOneAddressPerWindow();
  void outbox_successResetsBackoffAndRemembersAddress();
  void outbox_forwardRefusesInBackoffWithoutQueuing();
  void outbox_forwardNeverEnqueuesInBackoffEvenAtBoundary();
  void outbox_reachableFailureResettlesOnRetry();
  void outbox_forwardUnknownReturnsWithinGrace();
  void outbox_forwardTimeoutWithdrawsQueuedKey();
  void outbox_reachableForwardReportsOnlyCompletedSends();
  void outbox_expiredKeyDiscardedBeforeConnect();
  void outbox_failureInvokesHandlerOnceAndKeepsStickyLine();
  void outbox_discardKeysLeavesOtherLines();
  void outbox_queueIsCapped();
  void protocol_keyCarriesSeqAndSentAt();
  void protocol_keyClearAllRoundTrips();
  void keyReceive_dropsStaleDownKeepsUp();
  void keyReceive_clearAllInvokesHandlerAfterGating();
  void keyLaneFailure_resyncsLedgerAndPostsStickyClearAll();
  void relayStop_forwardedHoldsAreReleasedOnTheKeyLane();
  void rescue_discardsQueuedKeysAndResyncsLedger();
  void heartbeat_doesNotBlockOnUnreachablePeers();
  void keyForward_returnsWithinGraceWhenPeerUnreachable();
  void keyForward_followsRunningRoleNotElection();
  void relayReconciler_followsRunningRoleNotElection();
  void mesh_handlerThreadsAreBounded();
};

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

// TEST-NET-1 addresses: connects time out rather than being refused.
constexpr const char *kBlackholeA = "240.0.0.1";
constexpr const char *kBlackholeB = "240.0.0.2";

struct FakeClock
{
  double now = 0.0;
  PeerOutbox::Clock fn()
  {
    return [this] { return now; };
  }
};

struct FakeTransport
{
  std::vector<std::string> hosts; // every attempt, in order
  std::function<bool(const std::string &host)> okFor = [](const std::string &) { return false; };
  PeerOutbox::Transport fn()
  {
    return [this](const std::string &host, const std::string &, std::string *reply) {
      hosts.push_back(host);
      const bool ok = okFor(host);
      if (ok && reply != nullptr) {
        *reply = "ok";
      }
      return ok;
    };
  }
};

//! Relay monitor double: records start/stop so the reconciler is observable,
//! and the boundary hooks so resyncs are.
struct FakeKeyboardRelay : public deskflow::coordination::IKeyboardRelayMonitor
{
  std::atomic<int> starts{0};
  std::atomic<bool> live{false};
  std::atomic<int> resyncs{0};
  std::vector<KeyButton> heldOnStop; //!< reported to the sink at stop()
  ForwardedReleaseSink sink;

  bool start(RelayPassThroughQuery, KeyForwardSend) override
  {
    ++starts;
    live = true;
    return true;
  }
  void stop() override
  {
    live = false;
    if (!heldOnStop.empty() && sink) {
      sink(heldOnStop);
      heldOnStop.clear();
    }
  }
  bool running() const override
  {
    return live;
  }
  void setForwardedReleaseSink(ForwardedReleaseSink newSink) override
  {
    sink = std::move(newSink);
  }
  void releaseForwardedLocally() override
  {
    ++resyncs;
  }
};

//! Poll \p condition for up to \p timeoutMs (worker ticks are 1 s).
template <typename Condition> bool waitFor(Condition condition, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

double elapsedMs(const std::chrono::steady_clock::time_point &since)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

int connectLoopback(int port)
{
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd < 0) {
    return -1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    return -1;
  }
  return fd;
}

void closeFd(int fd)
{
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

} // namespace

void CoordinatorTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void CoordinatorTests::cleanupTestCase()
{
  g_arch.reset();
}

void CoordinatorTests::outbox_backoffDoublesToCapAndTriesOneAddressPerWindow()
{
  FakeClock clock;
  FakeTransport transport; // always fails: the peer is asleep
  PeerOutbox outbox("10.0.0.5", "peer.local", transport.fn(), clock.fn());
  QCOMPARE(outbox.state(), PeerOutbox::State::Unknown);

  // First attempt (state Unknown): both addresses are tried once, LAN first.
  outbox.post("claim");
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), static_cast<size_t>(2));
  QCOMPARE(transport.hosts[0], std::string("peer.local"));
  QCOMPARE(transport.hosts[1], std::string("10.0.0.5"));
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(outbox.nextAttemptAt(), 1.0);

  // Inside the window nothing is attempted, however much is posted.
  clock.now = 0.5;
  outbox.post("claim");
  outbox.post("fleet");
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), static_cast<size_t>(2));

  // Each later window costs exactly one connect, alternating addresses,
  // and the delay doubles 1 -> 2 -> 4 -> 8 -> 16 -> 30 (cap).
  const double expectedDelays[] = {2.0, 4.0, 8.0, 16.0, 30.0, 30.0};
  size_t attempts = transport.hosts.size();
  std::string previousWindowHost;
  for (const double delay : expectedDelays) {
    clock.now = outbox.nextAttemptAt();
    outbox.post("claim");
    outbox.pump(clock.now);
    QCOMPARE(transport.hosts.size(), attempts + 1);
    QVERIFY(transport.hosts.back() != previousWindowHost);
    QCOMPARE(outbox.nextAttemptAt(), clock.now + delay);
    previousWindowHost = transport.hosts.back();
    attempts = transport.hosts.size();
  }
  // A failed attempt discards what was queued behind it.
  QVERIFY(outbox.idle());
}

void CoordinatorTests::outbox_successResetsBackoffAndRemembersAddress()
{
  FakeClock clock;
  FakeTransport transport;
  PeerOutbox outbox("10.0.0.5", "peer.local", transport.fn(), clock.fn());

  // Two failed windows, then the peer wakes on its stable address only.
  outbox.post("a");
  outbox.pump(clock.now);
  clock.now = outbox.nextAttemptAt();
  outbox.post("b");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);

  transport.okFor = [](const std::string &host) { return host == "10.0.0.5"; };
  clock.now = outbox.nextAttemptAt();
  outbox.post("c");
  outbox.pump(clock.now);
  // The window's single attempt may land on either address; converge.
  if (outbox.state() != PeerOutbox::State::Reachable) {
    clock.now = outbox.nextAttemptAt();
    outbox.post("c");
    outbox.pump(clock.now);
  }
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  QCOMPARE(outbox.nextAttemptAt(), 0.0);
  QCOMPARE(outbox.preferredAddress(), std::string("10.0.0.5"));

  // Reachable: sends go out immediately and only to the known-good address.
  const size_t before = transport.hosts.size();
  std::string reply;
  outbox.post("query", [&reply](const std::string &line) { reply = line; });
  outbox.post("d");
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), before + 2);
  QCOMPARE(transport.hosts[before], std::string("10.0.0.5"));
  QCOMPARE(transport.hosts[before + 1], std::string("10.0.0.5"));
  QCOMPARE(reply, std::string("ok"));

  // A failure from Reachable restarts the schedule at the minimum delay.
  transport.okFor = [](const std::string &) { return false; };
  clock.now = 100.0;
  outbox.post("e");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(outbox.nextAttemptAt(), 101.0);
}

void CoordinatorTests::outbox_forwardRefusesInBackoffWithoutQueuing()
{
  FakeClock clock;
  FakeTransport transport;
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("a");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);

  // Inside the backoff window: refused, nothing queued.
  clock.now = 0.5;
  const auto started = std::chrono::steady_clock::now();
  QVERIFY(!outbox.forward("key", 500));
  QVERIFY(elapsedMs(started) < 50.0);
  QVERIFY(outbox.idle()); // a refused key is never delivered late
}

void CoordinatorTests::outbox_forwardNeverEnqueuesInBackoffEvenAtBoundary()
{
  // S2: the old "queue it anyway at the window boundary" path delivered
  // the key remotely AFTER the hook had typed it locally (and its Up only
  // locally): a phantom Down on the peer. Keys never ride a backoff lane.
  FakeClock clock;
  FakeTransport transport;
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("a");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(outbox.nextAttemptAt(), 1.0);

  transport.okFor = [](const std::string &) { return true; };
  clock.now = outbox.nextAttemptAt();
  const auto started = std::chrono::steady_clock::now();
  QVERIFY(!outbox.forward("key", 500));
  QVERIFY(elapsedMs(started) < 50.0);
  QVERIFY(outbox.idle());
  const size_t before = transport.hosts.size();
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), before); // nothing was queued

  // A regular post (heartbeat, probe, sticky resync) re-settles the lane.
  outbox.post("probe");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
}

void CoordinatorTests::outbox_reachableFailureResettlesOnRetry()
{
  FakeClock clock;
  FakeTransport transport;
  transport.okFor = [](const std::string &) { return true; };
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("a");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);

  // Reachable -> transient failure with more lines queued behind it: the
  // lane drops them (logged), enters the minimum backoff window and
  // reports forwards as local for exactly that window.
  transport.okFor = [](const std::string &) { return false; };
  clock.now = 10.0;
  outbox.post("b");
  outbox.post("c");
  outbox.post("d");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(outbox.nextAttemptAt(), 11.0);
  QVERIFY(outbox.idle());
  clock.now = 10.5;
  QVERIFY(!outbox.forward("key", 500));
  QVERIFY(outbox.idle());

  // Peer answers again: a posted line at the boundary re-settles
  // Reachable and resets the schedule (keys still refuse until then).
  transport.okFor = [](const std::string &) { return true; };
  clock.now = 11.0;
  QVERIFY(!outbox.forward("key", 500));
  outbox.post("probe");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  QCOMPARE(outbox.nextAttemptAt(), 0.0);
  outbox.start();
  QVERIFY(outbox.forward("key2", 500));
  outbox.stop();
}

void CoordinatorTests::outbox_forwardUnknownReturnsWithinGrace()
{
  FakeClock clock;
  FakeTransport slow;
  slow.okFor = [](const std::string &) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
  };
  PeerOutbox outbox("10.0.0.5", "", slow.fn(), clock.fn());
  outbox.start();

  // Unknown + slow peer: the hook gets an answer within the grace, not
  // after the connect. (The key's own connect is already in flight and
  // cannot be recalled; see outbox_forwardTimeoutWithdrawsQueuedKey for
  // the queued case.)
  auto started = std::chrono::steady_clock::now();
  QVERIFY(!outbox.forward("key", 20));
  QVERIFY(elapsedMs(started) < 150.0);
  outbox.stop();

  // Unknown + fast peer: the honest result arrives inside the grace.
  FakeTransport fast;
  fast.okFor = [](const std::string &) { return true; };
  PeerOutbox quick("10.0.0.5", "", fast.fn(), clock.fn());
  quick.start();
  QVERIFY(quick.forward("key", 1000));
  QCOMPARE(quick.state(), PeerOutbox::State::Reachable);
  // Reachable: the answer comes as soon as the send completes.
  started = std::chrono::steady_clock::now();
  QVERIFY(quick.forward("key", 1000));
  QVERIFY(elapsedMs(started) < 50.0);
  quick.stop();
}

void CoordinatorTests::outbox_reachableForwardReportsOnlyCompletedSends()
{
  // S3: a Reachable lane used to answer "delivered" before the send, so a
  // wedged peer collected keys that landed late in a burst or not at all.
  // Now the hook hears "delivered" only once THIS send completed.
  FakeClock clock;
  FakeTransport transport;
  transport.okFor = [](const std::string &) { return true; };
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("hello");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);

  // No lane thread: nothing can send, so the grace runs out and the key
  // is withdrawn -- reported local, never delivered.
  const auto started = std::chrono::steady_clock::now();
  QVERIFY(!outbox.forward("key", 20));
  QVERIFY(elapsedMs(started) < 150.0);
  QVERIFY(outbox.idle());
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), static_cast<size_t>(1)); // "hello" only

  // Send fails while Reachable: reported local (and the lane backs off).
  outbox.start();
  transport.okFor = [](const std::string &) { return false; };
  QVERIFY(!outbox.forward("key", 1000));
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  outbox.stop();
}

void CoordinatorTests::outbox_expiredKeyDiscardedBeforeConnect()
{
  // A key still queued past its deadline is discarded by pump() before any
  // connect -- counted, never sent. (forward() from a helper thread keeps
  // the job queued while this thread advances the clock past the deadline.)
  FakeClock clock;
  FakeTransport transport;
  transport.okFor = [](const std::string &) { return true; };
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("hello");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);

  std::atomic<bool> delivered{true};
  std::thread waiter([&outbox, &delivered] { delivered = outbox.forward("late-key", 200); });
  QVERIFY(waitFor([&outbox] { return !outbox.idle(); }, 500));
  clock.now += PeerOutbox::kKeyDeadlineS + 1.0;
  outbox.pump(clock.now);
  waiter.join();
  QVERIFY(!delivered.load());
  QCOMPARE(outbox.expiredKeys(), static_cast<uint64_t>(1));
  QCOMPARE(transport.hosts.size(), static_cast<size_t>(1)); // "hello" only
  QVERIFY(outbox.idle());
}

void CoordinatorTests::outbox_failureInvokesHandlerOnceAndKeepsStickyLine()
{
  FakeClock clock;
  FakeTransport transport;
  transport.okFor = [](const std::string &) { return true; };
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  int failures = 0;
  outbox.setFailureHandler([&outbox, &failures] {
    ++failures;
    outbox.postSticky("clear-all"); // what the coordinator does
  });
  outbox.post("hello");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);

  // Reachable -> failure: handler fires once; its sticky line survives
  // the drop of everything queued behind the failed line.
  transport.okFor = [](const std::string &) { return false; };
  clock.now = 10.0;
  outbox.post("a");
  outbox.post("b");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(failures, 1);
  QVERIFY(!outbox.idle()); // the sticky line waits

  // Retry fails again: no second handler call, sticky still waiting, and a
  // second identical sticky post is deduplicated.
  clock.now = outbox.nextAttemptAt();
  outbox.postSticky("clear-all");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(failures, 1);
  QVERIFY(!outbox.idle());

  // Peer answers: the sticky line is the first (and only) thing delivered.
  transport.okFor = [](const std::string &) { return true; };
  clock.now = outbox.nextAttemptAt();
  const size_t before = transport.hosts.size();
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  QCOMPARE(transport.hosts.size(), before + 1);
  QVERIFY(outbox.idle());
}

void CoordinatorTests::outbox_discardKeysLeavesOtherLines()
{
  FakeClock clock;
  FakeTransport transport;
  transport.okFor = [](const std::string &) { return true; };
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("hello");
  outbox.pump(clock.now);

  std::thread waiter([&outbox] { (void)outbox.forward("key", 200); });
  QVERIFY(waitFor([&outbox] { return !outbox.idle(); }, 500));
  outbox.post("rescue");
  outbox.discardKeys();
  waiter.join();
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), static_cast<size_t>(2)); // hello + rescue
}

void CoordinatorTests::outbox_forwardTimeoutWithdrawsQueuedKey()
{
  // The hook handled the key locally when the grace ran out; delivering
  // it later would type it twice. A key still waiting behind an in-flight
  // line is withdrawn.
  FakeClock clock;
  FakeTransport slow;
  slow.okFor = [](const std::string &) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
  };
  PeerOutbox outbox("10.0.0.5", "", slow.fn(), clock.fn());
  outbox.start();

  outbox.post("hello"); // picked up by the lane at once: in flight for 300 ms
  QVERIFY(waitFor([&outbox] { return !outbox.idle(); }, 500));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  QVERIFY(!outbox.forward("key", 20)); // queued behind "hello", times out
  QVERIFY(waitFor([&outbox] { return outbox.idle(); }, 2000));
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  outbox.stop();
  QCOMPARE(slow.hosts.size(), static_cast<size_t>(1)); // "hello" only
}

void CoordinatorTests::outbox_queueIsCapped()
{
  FakeClock clock;
  FakeTransport transport;
  transport.okFor = [](const std::string &) { return true; };
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  for (size_t i = 0; i < PeerOutbox::kMaxQueuedLines * 3; ++i) {
    outbox.post("line");
  }
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), PeerOutbox::kMaxQueuedLines);
}

void CoordinatorTests::heartbeat_doesNotBlockOnUnreachablePeers()
{
  CoordinatorConfig config;
  config.selfName = "server";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(
      std::string("a=") + kBlackholeA + "|" + kBlackholeB + ", b=" + kBlackholeB + "|" + kBlackholeA
  );

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  coordinator.m_localCoreRestartHook = [] {};
  QVERIFY(coordinator.start());
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_fleetState.server = "server";
    coordinator.m_election.becameServer();
  }

  // Before: 2 peers x 2 addresses x 700 ms connect = 2.8 s per heartbeat
  // on the worker thread. Now every send is a post.
  auto started = std::chrono::steady_clock::now();
  coordinator.broadcastClaim();
  coordinator.updateCursorHost("a");
  coordinator.probePeerMeshVersions();
  coordinator.requestFleetRescue();
  QVERIFY(elapsedMs(started) < 100.0);

  started = std::chrono::steady_clock::now();
  coordinator.stop(); // joins at most one in-flight connect per lane
  QVERIFY(elapsedMs(started) < 2000.0);
}

void CoordinatorTests::keyForward_returnsWithinGraceWhenPeerUnreachable()
{
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
    coordinator.m_fleetState.cursorHost = "hackintosh";
  }
  coordinator.setRunningRole(deskflow::coordination::Role::Client);

  // Inside the keyboard hook: unknown reachability resolves as "keep the
  // key local" within the grace, never after a 700 ms connect.
  const auto started = std::chrono::steady_clock::now();
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"), KeyForwardResult::Local
  );
  QVERIFY(elapsedMs(started) < 200.0);

  coordinator.stop();
}

void CoordinatorTests::keyForward_followsRunningRoleNotElection()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  // Not started (no lane threads): whether the key reached the forwarding
  // stage at all is observable through the first-forward log latch.
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
    coordinator.m_fleetState.cursorHost = "hackintosh";
  }

  // Election says Client, but the ServerApp of the previous epoch is still
  // running (dwell): Server::onKeyDown owns the keyboard, nothing forwards.
  coordinator.setRunningRole(Role::Server);
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"), KeyForwardResult::Local
  );
  QVERIFY(!coordinator.m_loggedKeyForward);
  coordinator.setRunningRole(Role::Init);
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"), KeyForwardResult::Local
  );
  QVERIFY(!coordinator.m_loggedKeyForward);

  // Only a running ClientApp forwards (Unknown reachability + no lane:
  // reported local, but the key went to the outbox).
  coordinator.setRunningRole(Role::Client);
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"), KeyForwardResult::Local
  );
  QVERIFY(coordinator.m_loggedKeyForward);
}

void CoordinatorTests::protocol_keyCarriesSeqAndSentAt()
{
  const auto line = protocol::encodeKey("tiny11", Message::KeyPhase::Down, 65, 0, 1, "en", "tok", 42, 1700000000123);
  const auto message = protocol::decode(line);
  QCOMPARE(message.type, Message::Type::Key);
  QCOMPARE(message.seq, static_cast<int64_t>(42));
  QCOMPARE(message.keySentAtMs, static_cast<int64_t>(1700000000123));
  QCOMPARE(message.keyButton, static_cast<uint16_t>(1));

  // Legacy line without the stamps decodes with zeros (never judged stale).
  const auto legacy = protocol::decode(R"({"t":"key","from":"a","phase":"down","id":65,"mask":0,"button":1})");
  QCOMPARE(legacy.seq, static_cast<int64_t>(0));
  QCOMPARE(legacy.keySentAtMs, static_cast<int64_t>(0));
  QVERIFY(protocol::wallClockMs() > 1700000000000);
}

void CoordinatorTests::protocol_keyClearAllRoundTrips()
{
  const auto message = protocol::decode(protocol::encodeKeyClearAll("tiny11", "tok"));
  QCOMPARE(message.type, Message::Type::KeyClearAll);
  QCOMPARE(message.name, std::string("tiny11"));
  QCOMPARE(message.token, std::string("tok"));
}

void CoordinatorTests::keyReceive_dropsStaleDownKeepsUp()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "hackintosh";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList("tiny11=10.0.0.3");

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  coordinator.setRunningRole(Role::Server);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_fleetState.server = "hackintosh";
    coordinator.m_election.becameServer();
  }
  int injected = 0;
  events.addHandler(EventTypes::CoordinationKeyForward, events.getSystemTarget(), [&injected](const Event &) {
    ++injected;
  });
  const auto drain = [&events] {
    events.addEvent(Event(EventTypes::Quit));
    events.loop();
  };
  const auto key = [](Message::KeyPhase phase, int64_t sentAt) {
    return protocol::decode(protocol::encodeKey("tiny11", phase, 65, 0, 1, "en", "test-token", 1, sentAt));
  };
  const int64_t now = protocol::wallClockMs();
  const int64_t stale = now - protocol::kRelayKeyMaxAgeMs - 500;

  coordinator.handleKeyForwardMessage(key(Message::KeyPhase::Down, now));     // fresh: injected
  coordinator.handleKeyForwardMessage(key(Message::KeyPhase::Down, stale));   // stale Down: dropped
  coordinator.handleKeyForwardMessage(key(Message::KeyPhase::Repeat, stale)); // stale Repeat: dropped
  coordinator.handleKeyForwardMessage(key(Message::KeyPhase::Up, stale));     // stale Up: still released
  coordinator.handleKeyForwardMessage(key(Message::KeyPhase::Down, 0));       // legacy: injected
  drain();
  QCOMPARE(injected, 3);
  events.removeHandler(EventTypes::CoordinationKeyForward, events.getSystemTarget());
}

void CoordinatorTests::keyReceive_clearAllInvokesHandlerAfterGating()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "hackintosh";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList("tiny11=10.0.0.3");

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  int cleared = 0;
  coordinator.setKeyClearAllHandler([&cleared] { ++cleared; });

  const auto clearFrom = [](const char *from) {
    return protocol::decode(protocol::encodeKeyClearAll(from, "test-token"));
  };
  const auto noReply = [](const std::string &) {};

  // Client that is not the cursor host: ignored (it holds nothing for anyone).
  coordinator.setRunningRole(Role::Client);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient("10.0.0.3");
    coordinator.m_fleetState.cursorHost = "tiny11";
  }
  coordinator.onMessage(clearFrom("tiny11"), noReply);
  QCOMPARE(cleared, 0);

  // Server epoch: honoured from a configured peer only.
  coordinator.setRunningRole(Role::Server);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_fleetState.server = "hackintosh";
    coordinator.m_election.becameServer();
  }
  coordinator.onMessage(clearFrom("stranger"), noReply);
  QCOMPARE(cleared, 0);
  coordinator.onMessage(clearFrom("tiny11"), noReply);
  QCOMPARE(cleared, 1);
}

void CoordinatorTests::keyLaneFailure_resyncsLedgerAndPostsStickyClearAll()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  auto relay = std::make_unique<FakeKeyboardRelay>();
  auto *relayPtr = relay.get();
  coordinator.m_keyboardRelay = std::move(relay);
  QVERIFY(coordinator.start());
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
    coordinator.m_fleetState.cursorHost = "hackintosh";
  }
  coordinator.setRunningRole(Role::Client);

  // The key rides the (Unknown) lane, whose connect times out (~700 ms):
  // the lane fails while it was not in backoff, which is the moment the
  // peer may be left holding forwarded keys.
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"), KeyForwardResult::Local
  );
  auto *lane = coordinator.outboxByName("hackintosh");
  QVERIFY(lane != nullptr);
  QVERIFY(waitFor([lane] { return lane->state() == PeerOutbox::State::Backoff; }, 4000));
  QVERIFY(waitFor([relayPtr] { return relayPtr->resyncs.load() > 0; }, 1000));
  QCOMPARE(relayPtr->resyncs.load(), 1);
  QVERIFY(!lane->idle()); // the sticky KeyClearAll waits for the peer

  coordinator.stop();
  QCOMPARE(relayPtr->resyncs.load(), 1);
}

void CoordinatorTests::relayStop_forwardedHoldsAreReleasedOnTheKeyLane()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  auto relay = std::make_unique<FakeKeyboardRelay>();
  auto *relayPtr = relay.get();
  coordinator.m_keyboardRelay = std::move(relay);
  // The constructor wired its sink into the monitor it created; the fake
  // took that monitor's place, so wire it the same way.
  relayPtr->setForwardedReleaseSink([&coordinator](const std::vector<KeyButton> &buttons) {
    coordinator.postForwardedReleases(buttons);
  });
  // Not started: no lane thread, so what stop() posts stays observable.
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
    coordinator.m_fleetState.cursorHost = "hackintosh";
  }
  coordinator.setRunningRole(Role::Client);
  auto *lane = coordinator.outboxByName("hackintosh");
  QVERIFY(lane != nullptr);

  // Nothing forwarded yet: stop() has nowhere to send releases.
  relayPtr->heldOnStop = {0xA0};
  relayPtr->stop();
  QVERIFY(lane->idle());

  // A key was forwarded on this lane (the grace expires and the key is
  // withdrawn, but the lane is now the key destination).
  (void)coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en");
  lane->discardKeys();
  QVERIFY(lane->idle());
  relayPtr->heldOnStop = {0xA0, 0x5B};
  relayPtr->stop();
  QVERIFY(!lane->idle()); // two Ups queued (regular class: kept across backoff)
  lane->discardKeys();
  QVERIFY(!lane->idle()); // ... and not key class: a rescue does not drop them
}

void CoordinatorTests::rescue_discardsQueuedKeysAndResyncsLedger()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  coordinator.m_localCoreRestartHook = [] {};
  auto relay = std::make_unique<FakeKeyboardRelay>();
  auto *relayPtr = relay.get();
  coordinator.m_keyboardRelay = std::move(relay);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
    coordinator.m_fleetState.cursorHost = "hackintosh";
  }
  coordinator.setRunningRole(Role::Client);

  // Four Esc taps ride the lane (no lane thread: each is withdrawn after
  // its grace and reported local).
  for (int i = 0; i < deskflow::coordination::EscTapRescue::kTaps - 1; ++i) {
    QCOMPARE(
        coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyEscape, 0, 53, "en"), KeyForwardResult::Local
    );
  }
  // S6: the fifth is Swallowed -- consumed, NOT reported as forwarded, so
  // the hook records it Local and its Up never chases a hold on the peer;
  // and every forwarded hold is re-labelled Local for the restart.
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyEscape, 0, 53, "en"), KeyForwardResult::Swallowed
  );
  QCOMPARE(relayPtr->resyncs.load(), 1);
}

void CoordinatorTests::relayReconciler_followsRunningRoleNotElection()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.keyboardFollowCursor = true;
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  auto relay = std::make_unique<FakeKeyboardRelay>();
  auto *relayPtr = relay.get();
  coordinator.m_keyboardRelay = std::move(relay);
  QVERIFY(coordinator.start());

  // Election flipped to Client while the ServerApp still runs its dwell:
  // the reconciler must NOT start a relay under a live ServerApp.
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
  }
  coordinator.setRunningRole(Role::Server);
  QVERIFY(!waitFor([relayPtr] { return relayPtr->starts.load() > 0; }, 2500));
  QCOMPARE(relayPtr->starts.load(), 0);

  // The ClientApp epoch actually starts: the reconciler heals a missing
  // relay within a tick.
  coordinator.setRunningRole(Role::Client);
  QVERIFY(waitFor([relayPtr] { return relayPtr->starts.load() > 0; }, 2500));
  QVERIFY(relayPtr->running());

  // Election promotes to Server while the ClientApp still runs: the relay
  // stays (the ClientApp still needs it); it is stopped once a ServerApp
  // is the running epoch.
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameServer();
  }
  QVERIFY(!waitFor([relayPtr] { return !relayPtr->running(); }, 2500));
  coordinator.setRunningRole(Role::Server);
  QVERIFY(waitFor([relayPtr] { return !relayPtr->running(); }, 2500));

  coordinator.stop();
}

void CoordinatorTests::mesh_handlerThreadsAreBounded()
{
  int received = 0;
  CoordinationMesh mesh(
      0, "test-token", [&received](const Message &, const std::function<void(const std::string &)> &) { ++received; }
  );
  QVERIFY(mesh.start());

  // Idle connections park inside handleClient until the read timeout; the
  // pool, not the connection count, bounds the handler threads.
  std::vector<int> fds;
  for (int i = 0; i < CoordinationMesh::kHandlerPoolSize * 3; ++i) {
    const int fd = connectLoopback(mesh.port());
    if (fd >= 0) {
      fds.push_back(fd);
    }
  }
  QVERIFY(fds.size() > static_cast<size_t>(CoordinationMesh::kHandlerPoolSize));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QVERIFY(mesh.busyHandlers() > 0);
  QVERIFY(mesh.busyHandlers() <= CoordinationMesh::kHandlerPoolSize);

  for (const int fd : fds) {
    closeFd(fd);
  }
  const auto started = std::chrono::steady_clock::now();
  mesh.stop(); // joins the pool; pending connections are closed
  QVERIFY(elapsedMs(started) < 3000.0);
  QCOMPARE(mesh.busyHandlers(), 0);
}

QTEST_MAIN(CoordinatorTests)

#include "CoordinatorTests.moc"
