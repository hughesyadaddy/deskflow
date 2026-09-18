/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcClientTests.h"

#include "common/VersionInfo.h"
#include "gui/ipc/IpcClient.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>

using deskflow::gui::ipc::IpcClient;

namespace {

const auto kNoSuchSocket = QStringLiteral("deskflow-ipcclient-tests-nobody-listens");

class TestClient : public IpcClient
{
public:
  TestClient(const QString &socketName, int maxAttempts)
      : IpcClient(nullptr, socketName, QStringLiteral("test"), maxAttempts)
  {
  }
};

} // namespace

void IpcClientTests::forever_client_backs_off_and_never_gives_up()
{
  TestClient client(kNoSuchSocket, IpcClient::kRetryForever);
  QSignalSpy failedSpy(&client, &IpcClient::connectionFailed);

  client.connectToServer();
  // The first attempt fails at once; each failure arms the next delay of the schedule.
  QTRY_COMPARE(client.attemptCount(), 1);
  QCOMPARE(client.pendingRetryDelayMs(), IpcClient::kFirstRetryDelayMs);
  QTRY_COMPARE_WITH_TIMEOUT(client.attemptCount(), 2, 1000);
  QCOMPARE(client.pendingRetryDelayMs(), IpcClient::kFirstRetryDelayMs * 2);
  QTRY_COMPARE_WITH_TIMEOUT(client.attemptCount(), 3, 1500);
  QCOMPARE(client.pendingRetryDelayMs(), IpcClient::kFirstRetryDelayMs * 4);
  QCOMPARE(failedSpy.count(), 0);
  QVERIFY(!client.isConnected());

  client.disconnectFromServer();
}

void IpcClientTests::limited_client_fails_after_max_attempts()
{
  TestClient client(kNoSuchSocket, 3);
  QSignalSpy failedSpy(&client, &IpcClient::connectionFailed);

  client.connectToServer();
  QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3000);
  QCOMPARE(client.attemptCount(), 3);
  QCOMPARE(client.pendingRetryDelayMs(), -1);
}

void IpcClientTests::disconnect_cancels_pending_retry()
{
  TestClient client(kNoSuchSocket, IpcClient::kRetryForever);
  client.connectToServer();
  QTRY_COMPARE(client.attemptCount(), 1);
  QVERIFY(client.pendingRetryDelayMs() > 0);

  client.disconnectFromServer();
  QCOMPARE(client.pendingRetryDelayMs(), -1);
  QTest::qWait(IpcClient::kFirstRetryDelayMs * 3);
  QCOMPARE(client.attemptCount(), 1);
}

void IpcClientTests::forever_client_attaches_once_server_appears()
{
  const auto socketName = QStringLiteral("deskflow-ipcclient-tests-%1").arg(QCoreApplication::applicationPid());
  QLocalServer::removeServer(socketName);

  TestClient client(socketName, IpcClient::kRetryForever);
  QSignalSpy connectedSpy(&client, &IpcClient::connected);
  client.connectToServer();
  QTRY_VERIFY(client.attemptCount() >= 2);

  // The server (a kickstarted core, say) comes up late and answers the hello.
  QLocalServer server;
  QVERIFY(server.listen(socketName));
  connect(&server, &QLocalServer::newConnection, &server, [&server] {
    auto *socket = server.nextPendingConnection();
    connect(socket, &QLocalSocket::readyRead, socket, [socket] {
      if (socket->readAll().startsWith("hello=")) {
        socket->write(QStringLiteral("hello=%1\n").arg(kVersion).toUtf8());
      }
    });
  });

  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 10000);
  QVERIFY(client.isConnected());
  QCOMPARE(client.pendingRetryDelayMs(), -1);
  client.disconnectFromServer();
  server.close();
}

QTEST_MAIN(IpcClientTests)
