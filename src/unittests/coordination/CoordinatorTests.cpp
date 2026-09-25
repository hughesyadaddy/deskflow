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
#include "common/ExitCodes.h"
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
  void outbox_forwardTimeoutSwallowsInFlightKey();
  void outbox_reachableForwardReportsOnlyCompletedSends();
  void outbox_expiredKeyDiscardedBeforeConnect();
  void outbox_failureInvokesHandlerOnceAndKeepsStickyLine();
  void outbox_discardKeysLeavesOtherLines();
  void outbox_queueIsCapped();
  void protocol_keyCarriesSeqAndSentAt();
  void protocol_keyClearAllRoundTrips();
  void keyReceive_ignoresWallClockAndDropsDuplicateSeq();
  void keyReceive_clearAllInvokesHandlerAfterGating();
  void keyLaneFailure_resyncsLedgerAndPostsStickyClearAll();
  void relayStop_forwardedHoldsAreReleasedOnTheKeyLane();
  void rescue_discardsQueuedKeysAndResyncsLedger();
  void rescue_duplicateDeliveryRestartsOnce();
  void stopAll_messageStopsLocallyOnce();
  void stopAll_tenEscBurstBroadcastsAndStopsLocallyOnce();
  void fleetCommands_unknownSourceIsDropped();
  void fleetCommands_knownPeerSourceIsAccepted();
  void offLoop_tenEscWithBlockedEventLoop_stopsAll();
  void offLoop_fiveEscWithBlockedEventLoop_exitsAfterAckTimeout();
  void offLoop_fiveEscWithLiveEventLoop_noExit();
  void claim_duplicateDeliveryEvaluatedOnce();
  void heartbeat_doesNotBlockOnUnreachablePeers();
  void keyForward_returnsWithinGraceWhenPeerUnreachable();
  void keyForward_followsRunningRoleNotElection();
  void relayReconciler_followsRunningRoleNotElection();
  void mesh_handlerThreadsAreBounded();

private:
  //! Nested (friend access): a seat whose event loop is never serviced.
  struct BlockedLoopSeat;
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

//! Decode \p line as if it arrived from \p source (the transport stamps it).
Message decodeFrom(const std::string &line, const std::string &source)
{
  Message message = protocol::decode(line);
  message.sourceAddress = source;
  return message;
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
  // after the connect. The key's own connect is already in flight and
  // cannot be recalled, so the answer is "delivered" (swallow locally; see
  // outbox_forwardTimeoutSwallowsInFlightKey) -- never "local", which typed
  // it on both machines.
  auto started = std::chrono::steady_clock::now();
  QVERIFY(outbox.forward("key", 20));
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

void CoordinatorTests::outbox_forwardTimeoutSwallowsInFlightKey()
{
  // The grace ran out while the key's OWN send was in flight (connect +
  // alternate-address retry). It cannot be recalled and lands on the peer a
  // moment later, so the hook must swallow it: "local" typed it twice.
  FakeClock clock;
  FakeTransport slow;
  std::atomic<bool> succeed{true};
  slow.okFor = [&succeed](const std::string &) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return succeed.load();
  };
  PeerOutbox outbox("10.0.0.5", "10.0.0.6", slow.fn(), clock.fn());
  int failures = 0;
  outbox.setFailureHandler([&failures] { ++failures; });
  outbox.start();

  const auto started = std::chrono::steady_clock::now();
  QVERIFY(outbox.forward("key", 20)); // in flight past the grace: delivered
  QVERIFY(elapsedMs(started) < 150.0);
  QVERIFY(waitFor([&outbox] { return outbox.idle(); }, 2000));
  QCOMPARE(outbox.state(), PeerOutbox::State::Reachable);
  QCOMPARE(slow.hosts.size(), static_cast<size_t>(1)); // sent exactly once
  QCOMPARE(failures, 0);

  // Same shape, but the in-flight send then fails on both addresses: still
  // swallowed (a lost key beats a doubled one), and the lane's failure
  // handler fires so the peer gets a resync (KeyClearAll).
  succeed = false;
  QVERIFY(outbox.forward("key2", 20));
  QVERIFY(waitFor([&outbox] { return outbox.state() == PeerOutbox::State::Backoff; }, 2000));
  QVERIFY(waitFor([&failures] { return failures == 1; }, 1000));
  QCOMPARE(slow.hosts.size(), static_cast<size_t>(3)); // key2 on lan, then ip
  outbox.stop();
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

void CoordinatorTests::keyReceive_ignoresWallClockAndDropsDuplicateSeq()
{
  // The sender swallows a key the moment its lane accepts it, so any drop
  // here loses the keystroke outright. A wall-clock age gate dropped every
  // Down from a VM guest whose clock had drifted (keystrokes vanished);
  // only the sender's seq may reject a delivery, and only a duplicate or
  // reordered Down/Repeat.
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "hackintosh";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList("tiny11=10.0.0.3,macbookpro=10.0.0.4");

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
  const auto drain = [&events, &injected] {
    events.addEvent(Event(EventTypes::Quit));
    events.loop();
    const int n = injected;
    injected = 0;
    return n;
  };
  const auto key = [](const char *from, Message::KeyPhase phase, int64_t seq, int64_t sentAt) {
    return protocol::decode(protocol::encodeKey(from, phase, 65, 0, 1, "en", "test-token", seq, sentAt));
  };
  const int64_t now = protocol::wallClockMs();
  const int64_t skewed = now - protocol::kRelayKeyMaxAgeMs - 5000; // a guest clock 5 s behind
  const int64_t ahead = now + 60000;                                // or a minute ahead

  // Wall clock is not judged: skewed Downs are injected.
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 1, skewed));
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Repeat, 2, skewed));
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 3, ahead));
  QCOMPARE(drain(), 3);

  // Duplicate / reordered Down or Repeat: dropped. Per sender: another
  // peer's low seq is its own counter.
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 3, now));   // duplicate
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Repeat, 2, now)); // reordered
  coordinator.handleKeyForwardMessage(key("macbookpro", Message::KeyPhase::Down, 1, now));
  QCOMPARE(drain(), 1);

  // An Up is never dropped, whatever its seq (a late release is idempotent
  // and always safer than a key left held) -- and it does not move the
  // watermark backwards.
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Up, 2, 0));
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 3, now)); // still a duplicate
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 4, now));
  QCOMPARE(drain(), 2);

  // Legacy sender (no seq): never judged.
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 0, 0));
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 0, 0));
  QCOMPARE(drain(), 2);

  // The sender's core restarted (hello): its counter starts over.
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 1, now));
  QCOMPARE(drain(), 0);
  const auto hello = protocol::decode(protocol::encodeHello(deskflow::coordination::kMeshProtocolVersion, "tiny11", "test-token"));
  coordinator.onMessage(hello, [](const std::string &) {});
  coordinator.handleKeyForwardMessage(key("tiny11", Message::KeyPhase::Down, 1, now));
  QCOMPARE(drain(), 1);
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
  std::string clearedBy;
  coordinator.setKeyClearAllHandler([&cleared, &clearedBy](const std::string &sender) {
    ++cleared;
    clearedBy = sender;
  });

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
  QCOMPARE(clearedBy, std::string("tiny11")); // only THAT sender's keys are released
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

  // The key rides the (Unknown) lane behind the start-up hello probe, whose
  // connect times out (~700 ms): still queued when the grace runs out, the
  // key is withdrawn (Local), and the lane fails while it was not in
  // backoff, which is the moment the peer may be left holding forwarded
  // keys.
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

  coordinator.m_exitProcessHook = [](int) {};

  // The taps are COUNTED by the local input monitor (off the event loop);
  // the relay only consults the count. Four Esc taps ride the lane (no
  // lane thread: each is withdrawn after its grace and reported local).
  for (int i = 0; i < deskflow::coordination::RescueBurst::kRestartTaps - 1; ++i) {
    coordinator.onLocalKeyDown(kKeyEscape, 0);
    QCOMPARE(
        coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyEscape, 0, 53, "en"), KeyForwardResult::Local
    );
  }
  // S6: the fifth is Swallowed -- consumed, NOT reported as forwarded, so
  // the hook records it Local and its Up never chases a hold on the peer.
  coordinator.onLocalKeyDown(kKeyEscape, 0);
  QCOMPARE(
      coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyEscape, 0, 53, "en"), KeyForwardResult::Swallowed
  );
  // Nothing fires on the press: the burst is decided once it has ended,
  // and only then is every forwarded hold re-labelled Local for the restart.
  QCOMPARE(relayPtr->resyncs.load(), 0);
  coordinator.settleEscBurst(deskflow::coordination::EscTapRescue::Clock::now() + std::chrono::seconds(1));
  QCOMPARE(relayPtr->resyncs.load(), 1);
}

void CoordinatorTests::stopAll_messageStopsLocallyOnce()
{
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  Coordinator coordinator(config);
  int restarts = 0;
  int stops = 0;
  coordinator.m_localCoreRestartHook = [&restarts] { ++restarts; };
  coordinator.m_localStopAllHook = [&stops] { ++stops; };
  const auto reply = [](const std::string &) {};
  const std::string line = protocol::encodeStopAll("test-token");

  // Duplicate deliveries (ip + lan lanes) and a later repeat: one stop,
  // never a loop, never a restart.
  coordinator.onMessage(decodeFrom(line, kBlackholeA), reply);
  coordinator.onMessage(decodeFrom(line, kBlackholeA), reply);
  QCOMPARE(stops, 1);
  QCOMPARE(restarts, 0);
  coordinator.m_lastRescueAt = -1.0e9;
  coordinator.onMessage(decodeFrom(line, kBlackholeA), reply);
  QCOMPARE(stops, 1);
  // A stopping seat also ignores a burst of its own.
  coordinator.requestFleetStopAll();
  QCOMPARE(stops, 1);
}

void CoordinatorTests::stopAll_tenEscBurstBroadcastsAndStopsLocallyOnce()
{
  using deskflow::coordination::EscTapRescue;
  using deskflow::coordination::RescueBurst;
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  int restarts = 0;
  int stops = 0;
  coordinator.m_localCoreRestartHook = [&restarts] { ++restarts; };
  coordinator.m_localStopAllHook = [&stops] { ++stops; };
  auto relay = std::make_unique<FakeKeyboardRelay>();
  auto *relayPtr = relay.get();
  coordinator.m_keyboardRelay = std::move(relay);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_election.becameClient(kBlackholeA);
    coordinator.m_fleetState.cursorHost = "hackintosh";
  }
  coordinator.setRunningRole(Role::Client);

  coordinator.m_exitProcessHook = [](int) {};

  // Taps 1-4 ride the lane; from the 5th on the burst is a rescue in
  // progress and every Esc is swallowed -- but NOTHING fires on the way
  // to ten (no restart at five). The monitor counts, the relay consults.
  for (int i = 0; i < RescueBurst::kStopAllTaps; ++i) {
    coordinator.onLocalKeyDown(kKeyEscape, 0);
    const auto expected = i < RescueBurst::kRestartTaps - 1 ? KeyForwardResult::Local : KeyForwardResult::Swallowed;
    QCOMPARE(coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyEscape, 0, 53, "en"), expected);
    QCOMPARE(restarts, 0);
    QCOMPARE(stops, 0);
  }
  QCOMPARE(relayPtr->resyncs.load(), 0);

  coordinator.settleEscBurst(EscTapRescue::Clock::now() + std::chrono::seconds(1));
  QCOMPARE(stops, 1);
  QCOMPARE(restarts, 0);
  QCOMPARE(relayPtr->resyncs.load(), 1); // same boundary as the rescue
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(coordinator.m_stopAllTriggered);
  }

  // Already stopping: a second burst changes nothing.
  for (int i = 0; i < RescueBurst::kStopAllTaps; ++i) {
    coordinator.onLocalKeyDown(kKeyEscape, 0);
  }
  coordinator.settleEscBurst(EscTapRescue::Clock::now() + std::chrono::seconds(2));
  QCOMPARE(stops, 1);
  QCOMPARE(restarts, 0);
}

void CoordinatorTests::fleetCommands_unknownSourceIsDropped()
{
  // Without a token the mesh accepts any line from anyone (INADDR_ANY);
  // rescue/stopall are therefore gated on the source address belonging to
  // a configured peer. Anything else is dropped -- one line from a random
  // LAN host must never restart or stop the fleet.
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  Coordinator coordinator(config);
  int restarts = 0;
  int stops = 0;
  coordinator.m_localCoreRestartHook = [&restarts] { ++restarts; };
  coordinator.m_localStopAllHook = [&stops] { ++stops; };
  const auto reply = [](const std::string &) {};

  for (const char *source : {"192.0.2.99", "", "not-an-address", "::1"}) {
    coordinator.onMessage(decodeFrom(protocol::encodeStopAll(""), source), reply);
    coordinator.onMessage(decodeFrom(protocol::encodeRescue(""), source), reply);
  }
  QCOMPARE(stops, 0);
  QCOMPARE(restarts, 0);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(!coordinator.m_stopAllTriggered);
  }
}

void CoordinatorTests::fleetCommands_knownPeerSourceIsAccepted()
{
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "";
  // Both the stable and the LAN entry of a peer count, including the
  // IPv4-mapped form a dual-stack listener would report.
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA + "|" + kBlackholeB);

  Coordinator coordinator(config);
  int restarts = 0;
  int stops = 0;
  coordinator.m_localCoreRestartHook = [&restarts] { ++restarts; };
  coordinator.m_localStopAllHook = [&stops] { ++stops; };
  const auto reply = [](const std::string &) {};

  coordinator.onMessage(decodeFrom(protocol::encodeRescue(""), kBlackholeB), reply);
  QCOMPARE(restarts, 1);
  coordinator.m_lastRescueAt = -1.0e9;
  coordinator.onMessage(decodeFrom(protocol::encodeRescue(""), std::string("::ffff:") + kBlackholeA), reply);
  QCOMPARE(restarts, 2);
  coordinator.onMessage(decodeFrom(protocol::encodeStopAll(""), kBlackholeA), reply);
  QCOMPARE(stops, 1);
}

//! A server seat whose core event loop is never serviced (the wedge the
//! gesture exists for): the counter and both executors must not need it.
struct CoordinatorTests::BlockedLoopSeat
{
  EventQueue events; //!< set on the coordinator, never looped
  Coordinator coordinator;
  int restarts = 0;
  int stops = 0;
  std::atomic<int> exits{0};
  std::atomic<int> exitCode{-1};
  FakeKeyboardRelay *relay = nullptr;

  static CoordinatorConfig config()
  {
    CoordinatorConfig config;
    config.selfName = "hackintosh";
    config.meshPort = 0;
    config.token = "test-token";
    config.peers = deskflow::coordination::parsePeerList(std::string("tiny11=") + kBlackholeA);
    return config;
  }

  BlockedLoopSeat() : coordinator(config())
  {
    coordinator.setEventQueue(&events);
    coordinator.m_localCoreRestartHook = [this] { ++restarts; };
    coordinator.m_localStopAllHook = [this] { ++stops; };
    coordinator.m_exitProcessHook = [this](int code) {
      exitCode = code;
      ++exits;
    };
    coordinator.m_rescueAckTimeout = std::chrono::milliseconds(50);
    auto fake = std::make_unique<FakeKeyboardRelay>();
    relay = fake.get();
    coordinator.m_keyboardRelay = std::move(fake);
    coordinator.setRunningRole(deskflow::coordination::Role::Server);
  }

  void tap(int times)
  {
    for (int i = 0; i < times; ++i) {
      coordinator.onLocalKeyDown(kKeyEscape, 0);
    }
  }

  void settle()
  {
    coordinator.settleEscBurst(deskflow::coordination::EscTapRescue::Clock::now() + std::chrono::seconds(1));
  }
};

void CoordinatorTests::offLoop_tenEscWithBlockedEventLoop_stopsAll()
{
  BlockedLoopSeat seat;
  seat.tap(deskflow::coordination::RescueBurst::kStopAllTaps);
  QCOMPARE(seat.stops, 0);
  seat.settle();
  // Reached StopAll from ten observed taps with the loop never serviced.
  QCOMPARE(seat.stops, 1);
  QCOMPARE(seat.restarts, 0);
  QCOMPARE(seat.relay->resyncs.load(), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  QCOMPARE(seat.exits.load(), 0); // stop-all never arms the ack watchdog
}

void CoordinatorTests::offLoop_fiveEscWithBlockedEventLoop_exitsAfterAckTimeout()
{
  BlockedLoopSeat seat;
  seat.tap(deskflow::coordination::RescueBurst::kRestartTaps);
  seat.settle();
  // The restart request went out (peers + local IPC) without the loop...
  QCOMPARE(seat.restarts, 1);
  QVERIFY(seat.coordinator.m_rescueWatchdog.armed());
  // ...and since the loop never dispatches the probe, the process is
  // hard-exited non-zero so the supervisor relaunches the core.
  QVERIFY(waitFor([&seat] { return seat.exits.load() == 1; }, 2000));
  QCOMPARE(seat.exitCode.load(), s_exitFailed);
}

void CoordinatorTests::offLoop_fiveEscWithLiveEventLoop_noExit()
{
  BlockedLoopSeat seat;
  seat.tap(deskflow::coordination::RescueBurst::kRestartTaps);
  seat.settle();
  QCOMPARE(seat.restarts, 1);
  QVERIFY(seat.coordinator.m_rescueWatchdog.armed());
  // A live loop dispatches the probe: the watchdog stands down.
  seat.events.addEvent(Event(EventTypes::Quit));
  seat.events.loop();
  QVERIFY(waitFor([&seat] { return !seat.coordinator.m_rescueWatchdog.armed(); }, 2000));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  QCOMPARE(seat.exits.load(), 0);
}

void CoordinatorTests::rescue_duplicateDeliveryRestartsOnce()
{
  // A-8: the server has two Esc counters and older peers send every line to
  // both ip and lan, so the same rescue lands twice in the same millisecond.
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList(std::string("hackintosh=") + kBlackholeA);

  Coordinator coordinator(config);
  int restarts = 0;
  coordinator.m_localCoreRestartHook = [&restarts] { ++restarts; };
  const auto reply = [](const std::string &) {};
  const std::string line = protocol::encodeRescue("test-token");

  coordinator.onMessage(decodeFrom(line, kBlackholeA), reply);
  coordinator.onMessage(decodeFrom(line, kBlackholeA), reply);
  QCOMPARE(restarts, 1);

  coordinator.m_lastRescueAt = -1.0e9; // window elapsed
  coordinator.onMessage(decodeFrom(line, kBlackholeA), reply);
  QCOMPARE(restarts, 2);
}

void CoordinatorTests::claim_duplicateDeliveryEvaluatedOnce()
{
  using deskflow::coordination::Role;
  CoordinatorConfig config;
  config.selfName = "tiny11";
  config.meshPort = 0;
  config.token = "test-token";
  config.peers = deskflow::coordination::parsePeerList("hackintosh=10.0.0.2,macbookpro=10.0.0.3");

  Coordinator coordinator(config);
  const auto reply = [](const std::string &) {};
  const auto claim = [](const char *from, const char *ip, int64_t seq) {
    return protocol::decode(protocol::encodeClaim(from, ip, ip, seq, "test-token"));
  };

  coordinator.onMessage(claim("hackintosh", "10.0.0.2", 7), reply);
  QCOMPARE(coordinator.m_election.role(), Role::Client);
  QCOMPARE(coordinator.m_election.serverAddress(), std::string("10.0.0.2"));
  // decide() names the followed peer so pre-connect ordering never walks
  // our own addresses first.
  QCOMPARE(coordinator.m_fleetState.server, std::string("hackintosh"));

  // Same (peer, seq) again is dropped before the election sees it (not even
  // the seq merge runs); a new seq from the same peer is evaluated as usual.
  coordinator.onMessage(claim("hackintosh", "10.0.0.2", 7), reply);
  QCOMPARE(coordinator.m_election.seq(), 7);
  coordinator.m_lastClaimSeqBySender["hackintosh"] = 9;
  coordinator.onMessage(claim("hackintosh", "10.0.0.2", 9), reply);
  QCOMPARE(coordinator.m_election.seq(), 7);
  coordinator.onMessage(claim("hackintosh", "10.0.0.2", 10), reply);
  QCOMPARE(coordinator.m_election.seq(), 10);
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
