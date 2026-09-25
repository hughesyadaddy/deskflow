/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcServerLocalRestartTests.h"

#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/ipc/IpcServer.h"

#include <QLocalSocket>
#include <QSignalSpy>

#include <memory>

namespace {

Log g_log;
std::unique_ptr<Arch> g_arch;

class TestIpcServer final : public deskflow::core::ipc::IpcServer
{
public:
  TestIpcServer()
      : IpcServer(nullptr, QStringLiteral("deskflow-ipc-restart-test"), QStringLiteral("test"))
  {
  }

private:
  void processCommand(QLocalSocket *, const QString &, const QStringList &) override
  {
  }
};

} // namespace

void IpcServerLocalRestartTests::initTestCase()
{
  g_arch = std::make_unique<Arch>();
  g_log.setFilter(LogLevel::Level::Error);
}

void IpcServerLocalRestartTests::cleanupTestCase()
{
  g_arch.reset();
}

void IpcServerLocalRestartTests::emptyClients_stopsWithoutQueuingRestart()
{
  TestIpcServer server;
  QSignalSpy stopSpy(&server, &deskflow::core::ipc::IpcServer::stopProcessRequested);

  server.requestLocalCoreRestart();

  // A rescue that cannot restart must do nothing rather than destroy the
  // thing it was meant to rescue: with no GUI client (and, on Windows, no
  // daemon to relaunch us) the core is NOT stopped and nothing is queued
  // for a later client to replay.
  QCOMPARE(stopSpy.count(), 0);
  QCOMPARE(server.m_pendingMessages.size(), 0);
  QVERIFY(!server.m_pendingMessages.contains(QStringLiteral("restartCore")));
}

void IpcServerLocalRestartTests::withClient_broadcastsRestartWithoutStop()
{
  TestIpcServer server;
  QLocalSocket dummy;
  server.m_clients.insert(&dummy);

  QSignalSpy stopSpy(&server, &deskflow::core::ipc::IpcServer::stopProcessRequested);
  server.requestLocalCoreRestart();

  QCOMPARE(stopSpy.count(), 0);
  QCOMPARE(server.m_pendingMessages.size(), 0);
}

void IpcServerLocalRestartTests::broadcastCommandIfClients_emptyDropsWithoutQueue()
{
  TestIpcServer server;
  server.broadcastCommandIfClients(QStringLiteral("restartCore"));
  QCOMPARE(server.m_pendingMessages.size(), 0);

  server.broadcastCommand(QStringLiteral("hello"));
  QCOMPARE(server.m_pendingMessages.size(), 1);
  QCOMPARE(server.m_pendingMessages.front(), QStringLiteral("hello"));
}

QTEST_MAIN(IpcServerLocalRestartTests)
