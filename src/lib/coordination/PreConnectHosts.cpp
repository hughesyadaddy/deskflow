/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/PreConnectHosts.h"

#include "common/FleetCursor.h"

namespace deskflow::coordination {

namespace {

void addHost(QStringList &hosts, const std::string &address)
{
  if (address.empty()) {
    return;
  }
  const auto host = QString::fromStdString(address);
  if (!hosts.contains(host)) {
    hosts << host;
  }
}

} // namespace

QStringList
preConnectHostsFromFleet(const FleetState &fleet, const std::string &serverAddress, const std::string &selfName)
{
  using deskflow::common::namesEqual;
  QStringList hosts;

  for (const auto &peer : fleet.peers) {
    if (namesEqual(peer.name, fleet.server)) {
      addHost(hosts, peer.lan);
      addHost(hosts, peer.ip);
    }
  }
  addHost(hosts, serverAddress);
  for (const auto &peer : fleet.peers) {
    if (namesEqual(peer.name, selfName) || namesEqual(peer.name, fleet.server)) {
      continue;
    }
    addHost(hosts, peer.lan);
    addHost(hosts, peer.ip);
  }
  return hosts;
}

QStringList defaultPreConnectHosts(const std::string &serverAddress, const std::string &selfName, const PeerList &peers)
{
  using deskflow::common::namesEqual;
  QStringList hosts;
  addHost(hosts, serverAddress);
  for (const auto &peer : peers) {
    if (namesEqual(peer.name, selfName)) {
      continue; // connecting to ourselves only burns a connect timeout
    }
    addHost(hosts, peer.lan);
    addHost(hosts, peer.ip);
  }
  return hosts;
}

} // namespace deskflow::coordination
