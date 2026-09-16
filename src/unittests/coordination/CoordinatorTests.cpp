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
#include "coordination/CoordinationMesh.h"
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
using deskflow::coordination::Message;
using deskflow::coordination::PeerOutbox;

class CoordinatorTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void outbox_backoffDoublesToCapAndTriesOneAddressPerWindow();
  void outbox_successResetsBackoffAndRemembersAddress();
  void outbox_forwardRefusesInBackoffWithoutQueuing();
  void outbox_forwardInBackoffEnqueuesAtAttemptBoundary();
  void outbox_reachableFailureResettlesOnRetry();
  void outbox_forwardUnknownReturnsWithinGrace();
  void outbox_forwardTimeoutWithdrawsQueuedKey();
  void outbox_queueIsCapped();
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

//! Relay monitor double: records start/stop so the reconciler is observable.
struct FakeKeyboardRelay : public deskflow::coordination::IKeyboardRelayMonitor
{
  std::atomic<int> starts{0};
  std::atomic<bool> live{false};

  bool start(RelayPassThroughQuery, KeyForwardSend) override
  {
    ++starts;
    live = true;
    return true;
  }
  void stop() override
  {
    live = false;
  }
  bool running() const override
  {
    return live;
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

void CoordinatorTests::outbox_forwardInBackoffEnqueuesAtAttemptBoundary()
{
  // A client posts nothing else on the server lane between version probes
  // (15 s), so a key must be allowed to make the window's attempt itself;
  // otherwise one transient failure keeps keys local until the next probe.
  FakeClock clock;
  FakeTransport transport;
  PeerOutbox outbox("10.0.0.5", "", transport.fn(), clock.fn());
  outbox.post("a");
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Backoff);
  QCOMPARE(outbox.nextAttemptAt(), 1.0);

  // Window open: still reported as not delivered (the key stays local),
  // but the line is queued so the lane attempts now.
  transport.okFor = [](const std::string &) { return true; };
  clock.now = outbox.nextAttemptAt();
  const auto started = std::chrono::steady_clock::now();
  QVERIFY(!outbox.forward("key", 500));
  QVERIFY(elapsedMs(started) < 50.0);
  QVERIFY(!outbox.idle());

  const size_t before = transport.hosts.size();
  outbox.pump(clock.now);
  QCOMPARE(transport.hosts.size(), before + 1);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  QVERIFY(outbox.idle());

  // Re-settled: the next key is forwarded for real.
  QVERIFY(outbox.forward("key2", 500));
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

  // Peer answers again: the boundary attempt re-settles Reachable and
  // resets the schedule.
  transport.okFor = [](const std::string &) { return true; };
  clock.now = 11.0;
  QVERIFY(!outbox.forward("key", 500));
  outbox.pump(clock.now);
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  QCOMPARE(outbox.nextAttemptAt(), 0.0);
  QVERIFY(outbox.forward("key2", 500));
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
  // Reachable: no waiting at all.
  started = std::chrono::steady_clock::now();
  QVERIFY(quick.forward("key", 1000));
  QVERIFY(elapsedMs(started) < 50.0);
  quick.stop();
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
  QVERIFY(!coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"));
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
  QVERIFY(!coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"));
  QVERIFY(!coordinator.m_loggedKeyForward);
  coordinator.setRunningRole(Role::Init);
  QVERIFY(!coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"));
  QVERIFY(!coordinator.m_loggedKeyForward);

  // Only a running ClientApp forwards (Unknown reachability + no lane:
  // reported local, but the key went to the outbox).
  coordinator.setRunningRole(Role::Client);
  QVERIFY(!coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyTab, KeyModifierAlt, 1, "en"));
  QVERIFY(coordinator.m_loggedKeyForward);
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
