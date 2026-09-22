/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InstanceHandoff.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QThread>
#include <QLocalSocket>

namespace deskflow::gui {

namespace {
const QByteArray kShow = "show\n";
const QByteArray kQuit = "quit\n";
const QByteArray kAck = "ok\n";
} // namespace

InstanceHandoffServer::InstanceHandoffServer(QObject *parent) : QObject(parent), m_server{new QLocalServer(this)}
{
  connect(m_server, &QLocalServer::newConnection, this, &InstanceHandoffServer::onNewConnection);
}

bool InstanceHandoffServer::listen(const QString &socketName)
{
  // In case of a previous crash remove first
  QLocalServer::removeServer(socketName);
  return m_server->listen(socketName);
}

void InstanceHandoffServer::close()
{
  m_server->close();
}

void InstanceHandoffServer::onNewConnection()
{
  while (auto *socket = m_server->nextPendingConnection()) {
    auto handled = std::make_shared<bool>(false);
    auto handle = [this, socket, handled](const QByteArray &line) {
      if (*handled)
        return;
      *handled = true;
      if (line == kQuit.trimmed() && m_honoursQuit) {
        qInfo("another gui instance asked this one to quit (launchd takeover)");
        socket->write(kAck);
        socket->flush();
        Q_EMIT quitRequested();
      } else {
        Q_EMIT showRequested();
      }
      socket->disconnectFromServer();
    };
    connect(socket, &QLocalSocket::readyRead, this, [socket, handle] {
      if (socket->canReadLine())
        handle(socket->readLine().trimmed());
    });
    // Older builds connect and hang up without writing anything: that is "show".
    connect(socket, &QLocalSocket::disconnected, this, [socket, handle] {
      handle(QByteArray());
      socket->deleteLater();
    });
  }
}

bool InstanceHandoffServer::send(const QString &socketName, const QByteArray &line, int timeoutMs)
{
  QLocalSocket socket;
  socket.connectToServer(socketName);
  if (!socket.waitForConnected(timeoutMs)) {
    return false;
  }
  socket.write(line);
  socket.flush();
  if (line == kQuit) {
    if (!socket.waitForReadyRead(timeoutMs) || socket.readLine().trimmed() != kAck.trimmed()) {
      return false;
    }
  } else {
    socket.waitForBytesWritten(timeoutMs);
  }
  socket.disconnectFromServer();
  return true;
}

bool InstanceHandoffServer::requestShow(const QString &socketName, int timeoutMs)
{
  return send(socketName, kShow, timeoutMs);
}

bool InstanceHandoffServer::requestQuit(const QString &socketName, int timeoutMs)
{
  return send(socketName, kQuit, timeoutMs);
}

bool InstanceHandoffServer::requestQuitRetrying(const QString &socketName, int totalMs, int attemptMs)
{
  QElapsedTimer clock;
  clock.start();
  while (true) {
    if (send(socketName, kQuit, attemptMs)) {
      return true;
    }
    if (clock.elapsed() >= totalMs) {
      return false;
    }
    QThread::msleep(200);
  }
}

} // namespace deskflow::gui
