/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoordinatorFleetPublishTests.h"

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "coordination/Coordinator.h"
#include "coordination/CoordinationProtocol.h"
#include "coordination/FleetState.h"

#include <QTest>

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

constexpr int kTestMeshPort = 59871;
Log g_log;
std::unique_ptr<Arch> g_arch;

CoordinatorConfig testConfig()
{
  CoordinatorConfig config;
  config.selfName = "server";
  config.meshPort = kTestMeshPort;
  config.token = "test-token";
  config.meshVersion = 2;
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

QTEST_MAIN(CoordinatorFleetPublishTests)
