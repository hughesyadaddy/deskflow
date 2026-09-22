/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class QLocalServer;

namespace deskflow::gui {

/**
 * @brief The running GUI's end of the "<appid>-gui" local socket.
 *
 * A second GUI that lost the single-instance lock connects here. A bare
 * connection (older builds) or a "show" line raises the window; a "quit" line
 * asks this instance to step aside so a launchd-owned instance can take over,
 * and is honoured only when this instance is not launchd-owned itself.
 */
class InstanceHandoffServer : public QObject
{
  Q_OBJECT

public:
  explicit InstanceHandoffServer(QObject *parent = nullptr);

  bool listen(const QString &socketName);
  void close();

  //! A launchd-owned instance never quits for a peer; the peer is the duplicate.
  void setHonoursQuit(bool honours)
  {
    m_honoursQuit = honours;
  }

  //! Client side: ask the running instance to raise its window.
  static bool requestShow(const QString &socketName, int timeoutMs = 1000);
  //! Client side: ask the running instance to quit; true when it acknowledged.
  static bool requestQuit(const QString &socketName, int timeoutMs = 1000);
  //! requestQuit repeated until it is acknowledged or \p totalMs elapsed. The
  //! lock is taken in main() before MainWindow listens on this socket, so a
  //! peer that raced us may hold the lock and not be listening yet; one
  //! attempt would time out and the caller would give up on a live handoff.
  static bool requestQuitRetrying(const QString &socketName, int totalMs = 5000, int attemptMs = 1000);

Q_SIGNALS:
  void showRequested();
  void quitRequested();

private:
  static bool send(const QString &socketName, const QByteArray &line, int timeoutMs);
  void onNewConnection();

  QLocalServer *m_server;
  bool m_honoursQuit = true;
};

} // namespace deskflow::gui
