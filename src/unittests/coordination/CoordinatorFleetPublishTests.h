/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

#include <string>

namespace deskflow::coordination {
class Coordinator;
}

class Arch;

class CoordinatorFleetPublishTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void updateCursorHost_updatesFleetSnapshot();
  void publishFleetTopology_updatesLinksAndScreens();
  void serverIgnoresInboundFleetMessage();
  void clientMergesInboundFleetMessage();
  void hello_rejectsV1Peer();
  void followClaim_resolvesEmptyAddressFromPeers();
  void followClaim_dropsUnknownClaimWithoutAddress();
  void serverTakeover_continuesFleetSeq();
  void wakePeer_rateLimitsPerPeer();
  void wakePeer_ignoredForClientsAndPeersWithoutHints();

private:
  static void armAsServer(deskflow::coordination::Coordinator &coordinator, const std::string &selfName);
  static void armAsClient(deskflow::coordination::Coordinator &coordinator);
};
