/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "HealthReportTests.h"

#include "HealthReport.h"
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "coordination/CoordinationProtocol.h"
#include "coordination/Coordinator.h"

#include <QRegularExpression>
#include <QTest>

using namespace deskflow::coordination;
using namespace deskflow::core::health;

namespace {
std::unique_ptr<Arch> g_arch;
Log g_log;
} // namespace

void HealthReportTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void HealthReportTests::cleanupTestCase()
{
  g_arch.reset();
}

void HealthReportTests::lineHasEveryFieldInOrder()
{
  Snapshot snapshot;
  snapshot.seat = "macbookpro";
  snapshot.role = "client";
  snapshot.epoch = 7;
  snapshot.upSeconds = 3600;
  snapshot.stats.peersReachable = 1;
  snapshot.stats.peersTotal = 2;
  snapshot.stats.links = 4;
  snapshot.stats.meshRx = 12345;
  snapshot.stats.meshDup = 3;
  snapshot.stats.flipsLastHour = 2;
  snapshot.stats.rescuesLastHour = 1;
  snapshot.tap = "ok";
  snapshot.ax = "trusted";
  snapshot.guiIpc = true;
  snapshot.mouserBridge = "attached";

  const auto line = QString::fromStdString(formatLine(snapshot));
  QCOMPARE(
      line, QStringLiteral("health: seat=macbookpro role=client epoch=7 up=3600 peers=1/2 links=4 mesh_rx=12345 "
                           "mesh_dup=3 tap=ok ax=trusted gui_ipc=connected flips_1h=2 rescue_1h=1 "
                           "mouser_bridge=attached")
  );
  // grep contract: one line, key=value tokens only, no spaces inside values
  QVERIFY(!line.contains('\n'));
  static const QRegularExpression token(QStringLiteral("^health:( [a-z_0-9]+=[^ ]+)+$"));
  QVERIFY(token.match(line).hasMatch());

  snapshot.guiIpc = false;
  QVERIFY(QString::fromStdString(formatLine(snapshot)).contains(QStringLiteral(" gui_ipc=none ")));
}

void HealthReportTests::tapStateFollowsRole()
{
  QCOMPARE(QString::fromStdString(tapState(Role::Client, true)), QStringLiteral("ok"));
  QCOMPARE(QString::fromStdString(tapState(Role::Client, false)), QStringLiteral("missing"));
  QCOMPARE(QString::fromStdString(tapState(Role::Server, false)), QStringLiteral("idle"));
  QCOMPARE(QString::fromStdString(tapState(Role::Init, true)), QStringLiteral("idle"));
}

void HealthReportTests::mouserBridgeState()
{
  QCOMPARE(QString::fromStdString(deskflow::core::health::mouserBridgeState(false, false)), QStringLiteral("none"));
  QCOMPARE(QString::fromStdString(deskflow::core::health::mouserBridgeState(false, true)), QStringLiteral("none"));
  QCOMPARE(QString::fromStdString(deskflow::core::health::mouserBridgeState(true, false)), QStringLiteral("attached"));
  QCOMPARE(QString::fromStdString(deskflow::core::health::mouserBridgeState(true, true)), QStringLiteral("linked"));
}

void HealthReportTests::coordinatorCountsFlipsRescuesAndMesh()
{
  CoordinatorConfig config;
  config.selfName = "alpha";
  config.meshPort = 0;
  config.peers = parsePeerList("alpha=240.0.0.1,beta=240.0.0.2");
  EventQueue events;
  Coordinator coordinator(config);
  coordinator.setEventQueue(&events);
  coordinator.m_localCoreRestartHook = [] {};
  QVERIFY(coordinator.start());

  auto stats = coordinator.healthStats();
  QCOMPARE(stats.peersTotal, 1);
  QCOMPARE(stats.peersReachable, 0);
  QCOMPARE(stats.meshRx, 0ULL);
  QCOMPARE(stats.flipsLastHour, 0);
  QCOMPARE(stats.rescuesLastHour, 0);

  coordinator.decide(Role::Server, "");
  coordinator.decide(Role::Server, "", true); // same role: a restart, not a flip
  coordinator.decide(Role::Client, "127.0.0.2");
  stats = coordinator.healthStats();
  QCOMPARE(stats.flipsLastHour, 2);

  Message status;
  status.type = Message::Type::Status;
  coordinator.onMessage(status, [](const std::string &) {});
  coordinator.onMessage(status, [](const std::string &) {});
  stats = coordinator.healthStats();
  QCOMPARE(stats.meshRx, 2ULL);

  coordinator.requestFleetRescue();
  stats = coordinator.healthStats();
  QCOMPARE(stats.rescuesLastHour, 1);
  QCOMPARE(stats.flipsLastHour, 2);
}

QTEST_MAIN(HealthReportTests)
