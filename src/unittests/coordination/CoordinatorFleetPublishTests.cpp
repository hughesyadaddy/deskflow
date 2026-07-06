/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoordinatorFleetPublishTests.h"

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "coordination/CoordinationProtocol.h"
#include "coordination/Coordinator.h"
#include "coordination/FleetState.h"
#include "coordination/Peer.h"

#include <QTest>

#include <chrono>
#include <memory>

using deskflow::coordination::Coordinator;
using deskflow::coordination::CoordinatorConfig;
using deskflow::coordination::FleetFragment;
using deskflow::coordination::FleetLink;
using deskflow::coordination::FleetScreen;
using deskflow::coordination::Message;
using deskflow::coordination::Role;
namespace protocol = deskflow::coordination::protocol;

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

CoordinatorConfig testConfig()
{
  CoordinatorConfig config;
  config.selfName = "server";
  config.meshPort = 0; // ephemeral: parallel test runs must not collide
  config.token = "test-token";
  return config;
}

} // namespace

void CoordinatorFleetPublishTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void CoordinatorFleetPublishTests::cleanupTestCase()
{
  g_arch.reset();
}

void CoordinatorFleetPublishTests::armAsServer(Coordinator &coordinator, const std::string &selfName)
{
  std::scoped_lock lock{coordinator.m_mutex};
  coordinator.m_fleetState.server = selfName;
  coordinator.m_election.becameServer();
}

void CoordinatorFleetPublishTests::armAsClient(Coordinator &coordinator)
{
  std::scoped_lock lock{coordinator.m_mutex};
  coordinator.m_election.becameClient("10.0.0.5");
}

void CoordinatorFleetPublishTests::updateCursorHost_updatesFleetSnapshot()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  CoordinatorFleetPublishTests::armAsServer(coordinator, "server");

  coordinator.updateCursorHost("remote");

  const auto snapshot = coordinator.fleetSnapshot();
  QCOMPARE(snapshot.cursorHost, std::string("remote"));
  QCOMPARE(snapshot.cursorScreen, std::string("remote"));
  QVERIFY(snapshot.seq > 0);

  coordinator.stop();
}

void CoordinatorFleetPublishTests::publishFleetTopology_updatesLinksAndScreens()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  CoordinatorFleetPublishTests::armAsServer(coordinator, "server");

  coordinator.publishFleetTopology(
      {FleetLink{"server", "remote", "right"}}, {FleetScreen{"server"}, FleetScreen{"remote"}}
  );

  const auto snapshot = coordinator.fleetSnapshot();
  QCOMPARE(snapshot.links.size(), static_cast<size_t>(1));
  QCOMPARE(snapshot.screens.size(), static_cast<size_t>(2));
  QCOMPARE(snapshot.links.front().toScreen, std::string("remote"));
  QVERIFY(snapshot.seq > 0);

  coordinator.stop();
}

void CoordinatorFleetPublishTests::serverIgnoresInboundFleetMessage()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsServer(coordinator, "server");
  coordinator.publishFleetTopology(
      {FleetLink{"server", "remote", "right"}}, {FleetScreen{"server"}, FleetScreen{"remote"}}
  );
  const auto before = coordinator.fleetSnapshot();

  FleetFragment inbound;
  inbound.server = "intruder";
  inbound.seq = 99;
  inbound.links = {FleetLink{"server", "other", "left"}};
  inbound.screens = {FleetScreen{"server"}, FleetScreen{"other"}};
  const Message message = protocol::decode(protocol::encodeFleet(inbound, "test-token"));
  QVERIFY(message.type == Message::Type::Fleet);

  coordinator.handleFleetMessage(message);

  const auto after = coordinator.fleetSnapshot();
  QCOMPARE(after.seq, before.seq);
  QCOMPARE(after.links.size(), before.links.size());
  QCOMPARE(after.links.front().toScreen, before.links.front().toScreen);

  coordinator.stop();
}

void CoordinatorFleetPublishTests::clientMergesInboundFleetMessage()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsClient(coordinator);

  FleetFragment inbound;
  inbound.server = "server";
  inbound.seq = 7;
  inbound.cursorHost = "remote";
  inbound.cursorScreen = "remote";
  inbound.links = {FleetLink{"server", "remote", "right"}};
  inbound.screens = {FleetScreen{"server"}, FleetScreen{"remote"}};
  const Message message = protocol::decode(protocol::encodeFleet(inbound, "test-token"));
  QVERIFY(message.type == Message::Type::Fleet);

  coordinator.handleFleetMessage(message);

  const auto snapshot = coordinator.fleetSnapshot();
  QCOMPARE(snapshot.server, std::string("server"));
  QCOMPARE(snapshot.seq, static_cast<int64_t>(7));
  QCOMPARE(snapshot.cursorHost, std::string("remote"));
  QCOMPARE(snapshot.links.size(), static_cast<size_t>(1));
  QVERIFY(!snapshot.links.empty());

  coordinator.stop();
}

void CoordinatorFleetPublishTests::hello_rejectsV1Peer()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());

  Message inbound;
  inbound.type = Message::Type::Hello;
  inbound.meshVersion = 1;
  inbound.name = "legacy";

  std::string reply;
  coordinator.handleHelloMessage(inbound, [&](const std::string &line) { reply = line; });

  QVERIFY(reply.empty());
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(coordinator.m_versionMismatchPeers.contains("legacy"));
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::hello_acceptClearsVersionMismatch()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());

  // A previously mismatched peer that reappears at the current protocol
  // version must get a reply and be cleared from the mismatch list.
  coordinator.noteVersionMismatch("legacy");

  Message inbound;
  inbound.type = Message::Type::Hello;
  inbound.meshVersion = deskflow::coordination::kMeshProtocolVersion;
  inbound.name = "legacy";

  std::string reply;
  coordinator.handleHelloMessage(inbound, [&](const std::string &line) { reply = line; });

  QVERIFY(!reply.empty());
  QCOMPARE(protocol::decode(reply).type, Message::Type::Hello);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(!coordinator.m_versionMismatchPeers.contains("legacy"));
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::followClaim_resolvesEmptyAddressFromPeers()
{
  auto config = testConfig();
  config.selfName = "macbookpro";
  // Same-address ip/lan skips the LAN probe, keeping the test offline.
  config.peers.push_back({"hackintosh", "hackintosh.test.example", "hackintosh.test.example"});
  config.peers.push_back({"macbookpro", "macbookpro.test.example", "macbookpro.test.example"});

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());

  // Regression: a claim with empty ip/lan and mismatched casing
  // ("Hackintosh" vs peer entry "hackintosh") must resolve the address
  // from the configured peer list instead of following "" (which left
  // clients with server_ip=null and dead keyboard forwarding).
  Message claim;
  claim.type = Message::Type::Claim;
  claim.name = "Hackintosh";

  coordinator.followSender(claim);

  {
    std::scoped_lock lock{coordinator.m_mutex};
    QCOMPARE(coordinator.m_election.role(), Role::Client);
    QCOMPARE(coordinator.m_election.serverAddress(), std::string("hackintosh.test.example"));
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::followClaim_dropsUnknownClaimWithoutAddress()
{
  EventQueue events;
  Coordinator coordinator(testConfig());
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsServer(coordinator, "server");

  Message claim;
  claim.type = Message::Type::Claim;
  claim.name = "stranger";

  coordinator.followSender(claim);

  {
    std::scoped_lock lock{coordinator.m_mutex};
    QCOMPARE(coordinator.m_election.role(), Role::Server);
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::serverTakeover_continuesFleetSeq()
{
  auto config = testConfig();
  config.selfName = "macbookpro";

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsClient(coordinator);

  // As a client, merge the previous server's snapshot up to seq 6.
  FleetFragment inbound;
  inbound.server = "hackintosh";
  inbound.seq = 6;
  inbound.cursorHost = "hackintosh";
  inbound.links = {FleetLink{"hackintosh", "macbookpro", "left"}};
  inbound.screens = {FleetScreen{"hackintosh"}, FleetScreen{"macbookpro"}};
  coordinator.handleFleetMessage(protocol::decode(protocol::encodeFleet(inbound, "test-token")));
  QCOMPARE(coordinator.fleetSnapshot().seq, static_cast<int64_t>(6));

  // Take over as server: publishes must continue above the merged seq or
  // every peer (and our own merge) rejects them as stale, freezing the
  // fleet cursor on the previous server.
  coordinator.decide(Role::Server, {});
  coordinator.updateCursorHost("macbookpro");

  const auto snapshot = coordinator.fleetSnapshot();
  QVERIFY(snapshot.seq > 6);
  QCOMPARE(snapshot.server, std::string("macbookpro"));
  QCOMPARE(snapshot.cursorHost, std::string("macbookpro"));

  coordinator.stop();
}

void CoordinatorFleetPublishTests::wakePeer_rateLimitsPerPeer()
{
  auto config = testConfig();
  // Deliberately invalid MAC: counts as a wake hint (rate-limit state is
  // exercised) but sendWakeOnLan rejects it, so the test neither
  // broadcasts UDP nor spawns a child that could hold the test mesh port.
  config.peers = deskflow::coordination::parsePeerList("sleepy=10.0.0.9|sleepy.local|invalid-mac");

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsServer(coordinator, "server");

  coordinator.wakePeer("sleepy");
  std::chrono::steady_clock::time_point firstWakeAt;
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QCOMPARE(coordinator.m_lastWakeAt.size(), static_cast<size_t>(1));
    firstWakeAt = coordinator.m_lastWakeAt.at("sleepy");
  }

  // A second request inside the 30 s window must not re-fire (timestamp
  // unchanged). Case-insensitive name matching applies, as everywhere.
  coordinator.wakePeer("SLEEPY");
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QCOMPARE(coordinator.m_lastWakeAt.at("sleepy"), firstWakeAt);
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::wakePeer_refiresAfterRateLimitWindow()
{
  auto config = testConfig();
  config.peers = deskflow::coordination::parsePeerList("sleepy=10.0.0.9|sleepy.local|invalid-mac");

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsServer(coordinator, "server");

  coordinator.wakePeer("sleepy");
  std::chrono::steady_clock::time_point firstWakeAt;
  {
    std::scoped_lock lock{coordinator.m_mutex};
    firstWakeAt = coordinator.m_lastWakeAt.at("sleepy");
    // Simulate the 30 s window expiring; the limiter must re-arm or a
    // peer that failed to wake is never retried.
    coordinator.m_lastWakeAt["sleepy"] = firstWakeAt - std::chrono::seconds(31);
  }

  coordinator.wakePeer("sleepy");
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(coordinator.m_lastWakeAt.at("sleepy") > firstWakeAt - std::chrono::seconds(1));
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::rescueChord_forcesRelayLocalUntilCursorMoves()
{
  auto config = testConfig();
  config.selfName = "macbookpro";

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());
  armAsClient(coordinator);

  // Fleet cursor sits on a remote host: keys would normally forward.
  FleetFragment inbound;
  inbound.server = "hackintosh";
  inbound.seq = 1;
  inbound.cursorHost = "hackintosh";
  inbound.links = {FleetLink{"hackintosh", "macbookpro", "left"}};
  inbound.screens = {FleetScreen{"hackintosh"}, FleetScreen{"macbookpro"}};
  coordinator.handleFleetMessage(protocol::decode(protocol::encodeFleet(inbound, "test-token")));
  QVERIFY(!coordinator.relayPassThroughLocal());

  // Chord (Down) engages the override: every key stays local, nothing
  // forwards, whatever the fleet state says.
  constexpr KeyModifierMask chord = KeyModifierShift | KeyModifierControl | KeyModifierAlt;
  coordinator.sendKeyForward(Message::KeyPhase::Down, kKeyEscape, chord, 1, "en");
  QVERIFY(coordinator.relayPassThroughLocal());
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(coordinator.m_relayLocalOverride);
  }

  // Fresh authoritative cursor state (host changed) clears the override.
  inbound.seq = 2;
  inbound.cursorHost = "tiny11";
  coordinator.handleFleetMessage(protocol::decode(protocol::encodeFleet(inbound, "test-token")));
  QVERIFY(!coordinator.relayPassThroughLocal());
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(!coordinator.m_relayLocalOverride);
  }

  coordinator.stop();
}

void CoordinatorFleetPublishTests::keyForward_gatingMatrix()
{
  auto config = testConfig();
  config.selfName = "macbookpro";
  config.peers = deskflow::coordination::parsePeerList("hackintosh=10.0.0.1, macbookpro=10.0.0.2, tiny11=10.0.0.3");

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());

  int forwarded = 0;
  events.addHandler(EventTypes::CoordinationKeyForward, events.getSystemTarget(), [&forwarded](const Event &) {
    ++forwarded;
  });
  const auto drainEvents = [&events] {
    events.addEvent(Event(EventTypes::Quit));
    events.loop();
  };
  const auto keyFrom = [](const char *from) {
    return protocol::decode(
        protocol::encodeKey(from, deskflow::coordination::RelayKeyPhase::Down, 65, 0, 1, "en", "test-token")
    );
  };

  // Client that is NOT the fleet cursor host: key dropped.
  armAsClient(coordinator);
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_fleetState.cursorHost = "tiny11";
  }
  coordinator.handleKeyForwardMessage(keyFrom("hackintosh"));
  drainEvents();
  QCOMPARE(forwarded, 0);

  // Client that IS the cursor host: key injected -- but only from a
  // configured peer; unknown senders are dropped even with a valid token.
  {
    std::scoped_lock lock{coordinator.m_mutex};
    coordinator.m_fleetState.cursorHost = "macbookpro";
  }
  coordinator.handleKeyForwardMessage(keyFrom("hackintosh"));
  coordinator.handleKeyForwardMessage(keyFrom("stranger"));
  drainEvents();
  QCOMPARE(forwarded, 1);

  // Server epoch: key injected from known peers only.
  armAsServer(coordinator, "macbookpro");
  coordinator.handleKeyForwardMessage(keyFrom("tiny11"));
  coordinator.handleKeyForwardMessage(keyFrom("stranger"));
  drainEvents();
  QCOMPARE(forwarded, 2);

  events.removeHandler(EventTypes::CoordinationKeyForward, events.getSystemTarget());
  coordinator.stop();
}

void CoordinatorFleetPublishTests::wakePeer_ignoredForClientsAndPeersWithoutHints()
{
  auto config = testConfig();
  config.peers = deskflow::coordination::parsePeerList("plain=10.0.0.7, sleepy=10.0.0.9|sleepy.local|invalid-mac");

  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  QVERIFY(coordinator.start());

  // Clients never fire wake actions, even for peers with hints.
  armAsClient(coordinator);
  coordinator.wakePeer("sleepy");
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(coordinator.m_lastWakeAt.empty());
  }

  // The server ignores peers without mac/wakeCommand and unknown names.
  armAsServer(coordinator, "server");
  coordinator.wakePeer("plain");
  coordinator.wakePeer("unknown");
  {
    std::scoped_lock lock{coordinator.m_mutex};
    QVERIFY(coordinator.m_lastWakeAt.empty());
  }

  coordinator.stop();
}

QTEST_MAIN(CoordinatorFleetPublishTests)
