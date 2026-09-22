/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InstanceHandoffTests.h"

#include "gui/InstanceHandoff.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QThread>

using deskflow::gui::InstanceHandoffServer;

namespace {

QString uniqueSocketName()
{
  static int counter = 0;
  return QStringLiteral("deskflow-handoff-tests-%1-%2").arg(QCoreApplication::applicationPid()).arg(++counter);
}

// The client calls block (waitForConnected/waitForReadyRead), so the server side
// must run on another thread for a same-process test.
class ClientThread : public QThread
{
public:
  ClientThread(QString socketName, bool quit, int retryTotalMs = 0)
      : m_socketName(std::move(socketName)),
        m_quit(quit),
        m_retryTotalMs(retryTotalMs)
  {
  }
  bool result = false;

protected:
  void run() override
  {
    if (m_retryTotalMs > 0) {
      result = InstanceHandoffServer::requestQuitRetrying(m_socketName, m_retryTotalMs, 300);
    } else {
      result = m_quit ? InstanceHandoffServer::requestQuit(m_socketName, 3000)
                      : InstanceHandoffServer::requestShow(m_socketName, 3000);
    }
  }

private:
  QString m_socketName;
  bool m_quit;
  int m_retryTotalMs;
};

} // namespace

void InstanceHandoffTests::quit_is_honoured_by_unmanaged_instance()
{
  const auto name = uniqueSocketName();
  InstanceHandoffServer server;
  server.setHonoursQuit(true);
  QVERIFY(server.listen(name));
  QSignalSpy quitSpy(&server, &InstanceHandoffServer::quitRequested);
  QSignalSpy showSpy(&server, &InstanceHandoffServer::showRequested);

  ClientThread client(name, true);
  client.start();
  QTRY_COMPARE_WITH_TIMEOUT(quitSpy.count(), 1, 5000);
  QVERIFY(client.wait(5000));
  QVERIFY(client.result); // the peer saw the acknowledgement before taking the lock
  QCOMPARE(showSpy.count(), 0);
}

void InstanceHandoffTests::quit_is_refused_by_launchd_owned_instance()
{
  const auto name = uniqueSocketName();
  InstanceHandoffServer server;
  server.setHonoursQuit(false);
  QVERIFY(server.listen(name));
  QSignalSpy quitSpy(&server, &InstanceHandoffServer::quitRequested);
  QSignalSpy showSpy(&server, &InstanceHandoffServer::showRequested);

  ClientThread client(name, true);
  client.start();
  QTRY_COMPARE_WITH_TIMEOUT(showSpy.count(), 1, 5000);
  QVERIFY(client.wait(5000));
  QVERIFY(!client.result); // no acknowledgement: the caller must exit 0, not take over
  QCOMPARE(quitSpy.count(), 0);
}

void InstanceHandoffTests::show_and_legacy_bare_connection_raise_window()
{
  const auto name = uniqueSocketName();
  InstanceHandoffServer server;
  QVERIFY(server.listen(name));
  QSignalSpy quitSpy(&server, &InstanceHandoffServer::quitRequested);
  QSignalSpy showSpy(&server, &InstanceHandoffServer::showRequested);

  ClientThread client(name, false);
  client.start();
  QTRY_COMPARE_WITH_TIMEOUT(showSpy.count(), 1, 5000);
  QVERIFY(client.wait(5000));
  QVERIFY(client.result);

  // An older build connects and hangs up without a word.
  {
    QLocalSocket bare;
    bare.connectToServer(name);
    QVERIFY(bare.waitForConnected(3000));
    bare.disconnectFromServer();
  }
  QTRY_COMPARE_WITH_TIMEOUT(showSpy.count(), 2, 5000);
  QCOMPARE(quitSpy.count(), 0);
}

void InstanceHandoffTests::requests_fail_when_nothing_listens()
{
  const auto name = uniqueSocketName();
  QVERIFY(!InstanceHandoffServer::requestQuit(name, 500));
  QVERIFY(!InstanceHandoffServer::requestShow(name, 500));
}

// The launchd copy asks before the BTM copy's MainWindow listens: the retry
// must bridge that gap instead of reporting "would not hand over".
void InstanceHandoffTests::quit_retries_until_a_late_listener_appears()
{
  const auto name = uniqueSocketName();
  ClientThread client(name, true, 5000);
  client.start();
  QThread::msleep(700); // nothing listens yet; the first attempt(s) fail

  InstanceHandoffServer server;
  server.setHonoursQuit(true);
  QVERIFY(server.listen(name));
  QSignalSpy quitSpy(&server, &InstanceHandoffServer::quitRequested);
  QTRY_COMPARE_WITH_TIMEOUT(quitSpy.count(), 1, 6000);
  QVERIFY(client.wait(6000));
  QVERIFY(client.result);
}

void InstanceHandoffTests::quit_retry_gives_up_after_the_budget()
{
  const auto name = uniqueSocketName();
  QElapsedTimer clock;
  clock.start();
  QVERIFY(!InstanceHandoffServer::requestQuitRetrying(name, 600, 100));
  QVERIFY(clock.elapsed() >= 600);
  QVERIFY(clock.elapsed() < 3000);
}

QTEST_MAIN(InstanceHandoffTests)
