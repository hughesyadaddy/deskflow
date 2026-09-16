/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MouserLinkTests.h"

#include "base/ILogOutputter.h"
#include "base/Log.h"
#include "deskflow/MouserLink.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using deskflow::MouserLink;

namespace {

constexpr int kWait = 5000;

//! Loopback stand-in for Mouser's listener: records every line per
//! connection, answers hellos and pings.
class FakeMouser : public QObject
{
public:
  FakeMouser()
  {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = m_server.nextPendingConnection()) {
        const int index = static_cast<int>(m_sockets.size());
        m_sockets.push_back(socket);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, index] { drain(socket, index); });
      }
    });
    if (!m_server.listen(QHostAddress::LocalHost, 0)) {
      qFatal("fake mouser: listen failed");
    }
  }

  int port() const
  {
    return m_server.serverPort();
  }

  int connections() const
  {
    return static_cast<int>(m_sockets.size());
  }

  int count(const QString &needle) const
  {
    int n = 0;
    for (const auto &entry : m_lines) {
      if (entry.second.contains(needle)) {
        ++n;
      }
    }
    return n;
  }

  QJsonObject first(const QString &needle, int connection = -1) const
  {
    for (const auto &entry : m_lines) {
      if ((connection < 0 || entry.first == connection) && entry.second.contains(needle)) {
        return QJsonDocument::fromJson(entry.second.toUtf8()).object();
      }
    }
    return {};
  }

  void send(int connection, const QString &line)
  {
    m_sockets.at(connection)->write((line + QStringLiteral("\n")).toUtf8());
    m_sockets.at(connection)->flush();
  }

  void close(int connection)
  {
    m_sockets.at(connection)->disconnectFromHost();
  }

  QString helloReply = QStringLiteral(R"({"ok":true,"proto":2,"caps":["focus","hidr"]})");
  bool answerPings = true;

private:
  void drain(QTcpSocket *socket, int index)
  {
    while (socket->canReadLine()) {
      const QString line = QString::fromUtf8(socket->readLine()).trimmed();
      m_lines.emplace_back(index, line);
      if (line.contains(QStringLiteral("\"hello\""))) {
        socket->write((helloReply + QStringLiteral("\n")).toUtf8());
      } else if (answerPings && line.contains(QStringLiteral("\"ping\""))) {
        socket->write("{\"t\":\"pong\"}\n");
      }
    }
  }

  QTcpServer m_server;
  std::vector<QTcpSocket *> m_sockets;
  std::vector<std::pair<int, QString>> m_lines;
};

class CountingOutputter : public ILogOutputter
{
public:
  explicit CountingOutputter(QString needle) : m_needle(std::move(needle))
  {
  }
  void open(const QString &) override
  {
  }
  void close() override
  {
  }
  bool write(LogLevel::Level, const QString &message) override
  {
    if (message.contains(m_needle)) {
      ++hits;
    }
    return false;
  }
  std::atomic<int> hits{0};

private:
  QString m_needle;
};

MouserLink::Options fastOptions(int port, const QString &tokenFile)
{
  MouserLink::Options options;
  options.port = port;
  options.tokenFile = tokenFile.toStdString();
  options.appVersion = "test";
  options.initialBackoff = std::chrono::milliseconds(30);
  options.maxBackoff = std::chrono::milliseconds(200);
  options.pingInterval = std::chrono::milliseconds(200);
  options.protoMismatchSleep = std::chrono::milliseconds(600);
  options.legacyEnabled = false;
  return options;
}

void writeToken(const QString &path, const QByteArray &token = "secret")
{
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(token + "\n");
}

int freePort()
{
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const int port = probe.serverPort();
  probe.close();
  return port;
}

} // namespace

void MouserLinkTests::helloThenAttachRoleAndFocus()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;

  MouserLink link(fastOptions(mouser.port(), tokenFile));
  link.setRole(MouserLink::Role::Server);
  link.notifyFocus("hackintosh", true);
  link.start();

  QTRY_VERIFY_WITH_TIMEOUT(mouser.count(QStringLiteral("\"t\":\"focus\"")) >= 1, kWait);
  const auto hello = mouser.first(QStringLiteral("\"hello\""));
  QCOMPARE(hello[QStringLiteral("t")].toString(), QStringLiteral("hello"));
  QCOMPARE(hello[QStringLiteral("proto")].toInt(), 2);
  QCOMPARE(hello[QStringLiteral("app")].toString(), QStringLiteral("deskflow-core"));
  QCOMPARE(hello[QStringLiteral("ver")].toString(), QStringLiteral("test"));
  QCOMPARE(hello[QStringLiteral("role")].toString(), QStringLiteral("server"));
  QCOMPARE(hello[QStringLiteral("token")].toString(), QStringLiteral("secret"));
  QVERIFY(hello[QStringLiteral("pid")].toInt() > 0);
  QVERIFY(hello[QStringLiteral("caps")].toArray().contains(QStringLiteral("focus")));

  const auto attach = mouser.first(QStringLiteral("\"attach\""));
  QCOMPARE(attach[QStringLiteral("session_id")].toString().toStdString(), link.sessionId());
  QCOMPARE(mouser.first(QStringLiteral("\"role\""))[QStringLiteral("role")].toString(), QStringLiteral("server"));
  const auto focus = mouser.first(QStringLiteral("\"t\":\"focus\""));
  QCOMPARE(focus[QStringLiteral("screen")].toString(), QStringLiteral("hackintosh"));
  QCOMPARE(focus[QStringLiteral("here")].toBool(), true);
  QCOMPARE(link.mode(), MouserLink::Mode::Lego);
  QVERIFY(link.connected());

  link.stop("shutdown");
  QTRY_VERIFY_WITH_TIMEOUT(mouser.count(QStringLiteral("\"bye\"")) == 1, kWait);
  QCOMPARE(mouser.first(QStringLiteral("\"bye\""))[QStringLiteral("reason")].toString(), QStringLiteral("shutdown"));
}

void MouserLinkTests::protoMismatchSleepsAndLogsOnce()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;
  mouser.helloReply = QStringLiteral(R"({"ok":false,"reason":"proto"})");

  auto *outputter = new CountingOutputter(QStringLiteral("rejected proto"));
  CLOG->insert(outputter);
  const auto previousFilter = CLOG->getFilter();
  CLOG->setFilter(LogLevel::Level::Warning);

  auto options = fastOptions(mouser.port(), tokenFile);
  MouserLink link(options);
  link.start();

  QTRY_COMPARE_WITH_TIMEOUT(mouser.connections(), 1, kWait);
  QElapsedTimer sinceReject;
  sinceReject.start();
  QTRY_COMPARE_WITH_TIMEOUT(link.stats().protoMismatches.load(), 1, kWait);

  // No retry storm: the second attempt only lands after the mismatch sleep.
  QTest::qWait(static_cast<int>(options.protoMismatchSleep.count() / 2));
  QCOMPARE(mouser.connections(), 1);
  QTRY_COMPARE_WITH_TIMEOUT(mouser.connections(), 2, kWait);
  QVERIFY(sinceReject.elapsed() >= options.protoMismatchSleep.count() - 50);
  QTRY_COMPARE_WITH_TIMEOUT(link.stats().protoMismatches.load(), 2, kWait);

  QCOMPARE(link.stats().protoMismatchLogs.load(), 1);
  QCOMPARE(outputter->hits.load(), 1);
  QVERIFY(!link.connected());

  link.stop();
  CLOG->setFilter(previousFilter);
  CLOG->remove(outputter);
  delete outputter;
}

void MouserLinkTests::authRejectionRereadsTokenAfterDelay()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile, "stale");
  FakeMouser mouser;
  mouser.helloReply = QStringLiteral(R"({"ok":false,"reason":"auth"})");

  auto options = fastOptions(mouser.port(), tokenFile);
  options.authRetry = std::chrono::milliseconds(300);
  MouserLink link(options);
  link.start();

  QTRY_COMPARE_WITH_TIMEOUT(link.stats().authRejections.load(), 1, kWait);
  QElapsedTimer sinceReject;
  sinceReject.start();
  // Mouser rotated the token: the next hello must carry the fresh one.
  writeToken(tokenFile, "fresh");
  mouser.helloReply = QStringLiteral(R"({"ok":true,"proto":2})");

  QTRY_COMPARE_WITH_TIMEOUT(mouser.connections(), 2, kWait);
  QVERIFY(sinceReject.elapsed() >= options.authRetry.count() - 50);
  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 1, kWait);
  QCOMPARE(mouser.first(QStringLiteral("\"hello\""), 0)[QStringLiteral("token")].toString(), QStringLiteral("stale"));
  QCOMPARE(mouser.first(QStringLiteral("\"hello\""), 1)[QStringLiteral("token")].toString(), QStringLiteral("fresh"));
  QVERIFY(link.connected());

  link.stop();
}

void MouserLinkTests::byeSuppressesBackoff()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;

  auto options = fastOptions(mouser.port(), tokenFile);
  options.initialBackoff = std::chrono::milliseconds(800);
  options.maxBackoff = std::chrono::milliseconds(800);
  MouserLink link(options);
  link.start();

  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 1, kWait);
  QElapsedTimer sinceBye;
  sinceBye.start();
  mouser.send(0, QStringLiteral(R"({"t":"bye","reason":"restart"})"));
  mouser.close(0);

  // Mouser is restarting on purpose: reconnect at once, no backoff.
  QTRY_COMPARE_WITH_TIMEOUT(mouser.connections(), 2, kWait);
  QVERIFY2(sinceBye.elapsed() < options.initialBackoff.count() / 2, "reconnect waited for the backoff");
  QCOMPARE(link.stats().byesReceived.load(), 1);
  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 2, kWait);

  link.stop();
}

void MouserLinkTests::reconnectReusesSessionId()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;

  MouserLink link(fastOptions(mouser.port(), tokenFile));
  link.setRole(MouserLink::Role::Client);
  link.start();

  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 1, kWait);
  mouser.close(0); // crash, not bye: backoff applies, session id does not change
  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 2, kWait);

  const auto first = mouser.first(QStringLiteral("\"attach\""), 0)[QStringLiteral("session_id")].toString();
  const auto second = mouser.first(QStringLiteral("\"attach\""), 1)[QStringLiteral("session_id")].toString();
  QVERIFY(!first.isEmpty());
  QCOMPARE(second, first);
  QCOMPARE(first.toStdString(), link.sessionId());
  // Role is replayed on the new session too.
  QCOMPARE(mouser.first(QStringLiteral("\"role\""), 1)[QStringLiteral("role")].toString(), QStringLiteral("client"));

  link.stop();
}

void MouserLinkTests::focusSwitchSendsFocusNotDisconnect()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;

  MouserLink link(fastOptions(mouser.port(), tokenFile));
  link.setRole(MouserLink::Role::Server);
  link.start();
  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 1, kWait);

  for (int i = 0; i < 20; ++i) {
    link.notifyFocus(i % 2 == 0 ? "tiny11" : "hackintosh", i % 2 != 0);
  }
  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"t\":\"focus\"")), 20, kWait);
  QCOMPARE(mouser.count(QStringLiteral("disconnect")), 0);
  QCOMPARE(mouser.count(QStringLiteral("\"connect\"")), 0);
  const auto away = mouser.first(QStringLiteral("tiny11"));
  QCOMPARE(away[QStringLiteral("t")].toString(), QStringLiteral("focus"));
  QCOMPARE(away[QStringLiteral("here")].toBool(), false);
  QCOMPARE(mouser.connections(), 1);

  link.stop();
}

void MouserLinkTests::survivesServerEpochChurnWithOneAttach()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;

  MouserLink link(fastOptions(mouser.port(), tokenFile));
  link.start();

  // What Server::initMouserBridge / ~Server do per epoch: borrow the link,
  // register/clear the inbound handler, announce the role and seed focus.
  std::mutex mutex;
  std::vector<std::string> inbound;
  for (int cycle = 0; cycle < 50; ++cycle) {
    link.setInboundHandler([&](const std::string &line) {
      std::scoped_lock lock{mutex};
      inbound.push_back(line);
    });
    link.setRole(cycle % 2 == 0 ? MouserLink::Role::Server : MouserLink::Role::Client);
    link.notifyFocus("hackintosh", true);
    link.setInboundHandler({});
    link.setRole(MouserLink::Role::None);
  }
  link.setInboundHandler([&](const std::string &line) {
    std::scoped_lock lock{mutex};
    inbound.push_back(line);
  });
  link.setRole(MouserLink::Role::Server);

  QTRY_VERIFY_WITH_TIMEOUT(mouser.count(QStringLiteral("\"t\":\"focus\"")) >= 1, kWait);
  QTest::qWait(150);
  QCOMPARE(mouser.connections(), 1);
  QCOMPARE(mouser.count(QStringLiteral("\"attach\"")), 1);
  QCOMPARE(link.stats().attachesSent.load(), 1);
  QVERIFY(link.connected());

  // The session still carries Mouser's lines to the current Server.
  mouser.send(0, QStringLiteral(R"({"type":"connect","device":"mx"})"));
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        std::scoped_lock lock{mutex};
        return !inbound.empty();
      }(),
      kWait
  );
  {
    std::scoped_lock lock{mutex};
    QCOMPARE(inbound.back(), std::string(R"({"type":"connect","device":"mx"})"));
  }

  link.stop();
}

void MouserLinkTests::dropsAfterUnansweredPings()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token"));
  writeToken(tokenFile);
  FakeMouser mouser;
  mouser.answerPings = false;

  auto options = fastOptions(mouser.port(), tokenFile);
  options.pingInterval = std::chrono::milliseconds(60);
  MouserLink link(options);
  link.start();

  QTRY_COMPARE_WITH_TIMEOUT(mouser.count(QStringLiteral("\"attach\"")), 1, kWait);
  QTRY_VERIFY_WITH_TIMEOUT(link.stats().pingDrops.load() >= 1, kWait);
  QTRY_VERIFY_WITH_TIMEOUT(mouser.connections() >= 2, kWait);
  QVERIFY(mouser.count(QStringLiteral("\"ping\"")) >= 3);

  link.stop();
}

void MouserLinkTests::legacyListenerWhenTokenFileAbsent()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token")); // never written

  auto options = fastOptions(1, tokenFile);
  options.legacyEnabled = true;
  options.legacyServerEnabled = true;
  options.legacyServerPort = freePort();
  options.legacyServerToken = "old-secret";
  MouserLink link(options);
  link.setRole(MouserLink::Role::Server);
  link.notifyFocus("hackintosh", true);
  link.start();

  QTRY_COMPARE_WITH_TIMEOUT(link.mode(), MouserLink::Mode::Legacy, kWait);

  // An un-upgraded Mouser dials the old bridge port with the v1 hello.
  QTcpSocket oldMouser;
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        oldMouser.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(options.legacyServerPort));
        return oldMouser.waitForConnected(200);
      }(),
      kWait
  );
  oldMouser.write("{\"type\":\"hello\",\"token\":\"old-secret\",\"version\":1}\n");
  QTRY_VERIFY_WITH_TIMEOUT(oldMouser.canReadLine(), kWait);
  const auto ack = QJsonDocument::fromJson(oldMouser.readLine().trimmed()).object();
  QCOMPARE(ack[QStringLiteral("ok")].toBool(), true);
  QCOMPARE(ack[QStringLiteral("version")].toInt(), 1);
  QTRY_VERIFY_WITH_TIMEOUT(link.connected(), kWait);

  // Focus arrives in the old vocabulary, and the old Mouser's lines reach
  // the inbound handler unchanged.
  QTRY_VERIFY_WITH_TIMEOUT(oldMouser.canReadLine(), kWait);
  const auto focus = QJsonDocument::fromJson(oldMouser.readLine().trimmed()).object();
  QCOMPARE(focus[QStringLiteral("type")].toString(), QStringLiteral("focus"));
  QCOMPARE(focus[QStringLiteral("local")].toBool(), true);

  std::mutex mutex;
  std::vector<std::string> inbound;
  link.setInboundHandler([&](const std::string &line) {
    std::scoped_lock lock{mutex};
    inbound.push_back(line);
  });
  oldMouser.write("{\"type\":\"event\",\"name\":\"gesture\"}\n");
  oldMouser.flush();
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        std::scoped_lock lock{mutex};
        return !inbound.empty();
      }(),
      kWait
  );
  {
    std::scoped_lock lock{mutex};
    QCOMPARE(inbound.front(), std::string(R"({"type":"event","name":"gesture"})"));
  }

  link.stop();
}

void MouserLinkTests::legacyConnectorTranslatesFocus()
{
  QTemporaryDir dir;
  const auto tokenFile = dir.filePath(QStringLiteral("bridge.token")); // never written
  FakeMouser oldMouser;
  oldMouser.helloReply = QStringLiteral(R"({"ok":true})");

  auto options = fastOptions(1, tokenFile);
  options.legacyEnabled = true;
  options.legacyClientEnabled = true;
  options.legacyClientPort = oldMouser.port();
  options.legacyClientToken = "old-secret";
  MouserLink link(options);
  link.setRole(MouserLink::Role::Client);
  link.start();

  QTRY_VERIFY_WITH_TIMEOUT(link.connected(), kWait);
  const auto hello = oldMouser.first(QStringLiteral("\"hello\""));
  QCOMPARE(hello[QStringLiteral("type")].toString(), QStringLiteral("hello"));
  QCOMPARE(hello[QStringLiteral("token")].toString(), QStringLiteral("old-secret"));
  QCOMPARE(hello[QStringLiteral("version")].toInt(), 1);
  QCOMPARE(oldMouser.count(QStringLiteral("attach")), 0);

  // Relayed lines keep their v1 shape; the new focus notices become the
  // connect/disconnect an old Mouser expects.
  link.deliver(R"({"type":"connect","device":"mx"})");
  link.deliver(R"({"t":"focus","screen":"tiny11","here":true})");
  link.deliver(R"({"type":"event","name":"gesture"})");
  link.deliver(R"({"t":"focus","screen":"hackintosh","here":false})");
  QTRY_COMPARE_WITH_TIMEOUT(oldMouser.count(QStringLiteral("disconnect")), 1, kWait);
  QCOMPARE(oldMouser.count(QStringLiteral("\"connect\"")), 1);
  QCOMPARE(oldMouser.count(QStringLiteral("gesture")), 1);
  QCOMPARE(oldMouser.count(QStringLiteral("\"t\":\"focus\"")), 0);

  link.stop();
}

QTEST_MAIN(MouserLinkTests)
