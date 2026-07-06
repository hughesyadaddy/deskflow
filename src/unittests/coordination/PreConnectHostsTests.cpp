/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PreConnectHostsTests.h"

#include "coordination/PreConnectHosts.h"

#include <QTest>

using deskflow::coordination::defaultPreConnectHosts;
using deskflow::coordination::FleetPeer;
using deskflow::coordination::FleetState;
using deskflow::coordination::parsePeerList;
using deskflow::coordination::preConnectHostsFromFleet;

namespace {

FleetState threeMachineFleet()
{
  FleetState fleet;
  fleet.server = "hackintosh";
  fleet.peers = {
      FleetPeer{"hackintosh", "hackintosh.ts.net", "192.168.1.10"},
      FleetPeer{"macbookpro", "macbookpro.ts.net", "macbookpro.local"},
      FleetPeer{"tiny11", "tiny11.ts.net", "192.168.1.100"},
  };
  return fleet;
}

} // namespace

void PreConnectHostsTests::fleet_serverLanFirstThenStable()
{
  // Candidate order is connect order: each dead candidate costs a connect
  // timeout, so the elected server's LAN address must come first.
  const auto hosts = preConnectHostsFromFleet(threeMachineFleet(), "hackintosh.ts.net", "macbookpro");

  QCOMPARE(
      hosts, QStringList(
                 {QStringLiteral("192.168.1.10"), QStringLiteral("hackintosh.ts.net"), QStringLiteral("192.168.1.100"),
                  QStringLiteral("tiny11.ts.net")}
             )
  );
}

void PreConnectHostsTests::fleet_excludesSelfCaseInsensitive()
{
  auto fleet = threeMachineFleet();
  fleet.peers[1].name = "MacBookPro"; // computerName casing mismatch

  const auto hosts = preConnectHostsFromFleet(fleet, {}, "macbookpro");

  QVERIFY(!hosts.contains(QStringLiteral("macbookpro.local")));
  QVERIFY(!hosts.contains(QStringLiteral("macbookpro.ts.net")));
}

void PreConnectHostsTests::fleet_dedupesWhenLanEqualsIp()
{
  FleetState fleet;
  fleet.server = "desktop";
  fleet.peers = {FleetPeer{"desktop", "10.0.0.2", "10.0.0.2"}};

  const auto hosts = preConnectHostsFromFleet(fleet, "10.0.0.2", "laptop");

  QCOMPARE(hosts, QStringList({QStringLiteral("10.0.0.2")}));
}

void PreConnectHostsTests::fleet_emptyFleetYieldsServerAddressOnly()
{
  const auto hosts = preConnectHostsFromFleet(FleetState{}, "10.0.0.9", "laptop");
  QCOMPARE(hosts, QStringList({QStringLiteral("10.0.0.9")}));
}

void PreConnectHostsTests::defaults_serverAddressFirstThenPeers()
{
  const auto peers = parsePeerList("desktop=10.0.0.2|desktop.local, laptop=10.0.0.3, tiny=10.0.0.4|10.0.0.4");
  const auto hosts = defaultPreConnectHosts("10.0.0.2", "laptop", peers);

  // Server address first, then LAN-then-stable per peer, self excluded,
  // duplicates (server address, lan == ip) removed.
  QCOMPARE(
      hosts, QStringList({QStringLiteral("10.0.0.2"), QStringLiteral("desktop.local"), QStringLiteral("10.0.0.4")})
  );
}

void PreConnectHostsTests::defaults_emptyServerAddress()
{
  const auto peers = parsePeerList("desktop=10.0.0.2");
  const auto hosts = defaultPreConnectHosts({}, "laptop", peers);
  QCOMPARE(hosts, QStringList({QStringLiteral("10.0.0.2")}));
}

QTEST_MAIN(PreConnectHostsTests)
