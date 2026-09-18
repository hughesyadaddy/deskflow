/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class QLocalSocket;

namespace deskflow::gui::ipc {

class IpcClient : public QObject
{
  Q_OBJECT

  // Represents underlying socket state and whether the server responded to the hello message.
  enum class State
  {
    Unconnected,
    Connecting,
    Connected,
    Disconnecting,
  };

public:
  //! @c maxAttempts of @c kRetryForever keeps retrying (with backoff) until connected or disconnectFromServer().
  static constexpr int kRetryForever = 0;
  static constexpr int kFirstRetryDelayMs = 250;
  static constexpr int kMaxRetryDelayMs = 4000;
  //! While retrying, the failure is logged at most this often (the first failure is always logged).
  static constexpr int kRetryLogIntervalMs = 60000;

  explicit IpcClient(QObject *parent, const QString &socketName, const QString &typeName, int maxAttempts);
  void connectToServer();
  void disconnectFromServer();

  bool isConnected() const
  {
    return m_state == State::Connected;
  }
  //! Connect attempts made since the last connectToServer().
  int attemptCount() const
  {
    return m_retryCount;
  }
  //! Delay of the pending retry in ms, or -1 when none is scheduled.
  int pendingRetryDelayMs() const
  {
    return m_retryTimer.isActive() ? m_retryTimer.interval() : -1;
  }

Q_SIGNALS:
  void connected();
  void connectionFailed();
  void serverShutdown();
  void versionMismatch();

private Q_SLOTS:
  void handleDisconnected();
  void handleErrorOccurred();
  void handleReadyRead();
  void sendHello();

protected:
  virtual void processCommand(const QString &command, const QStringList &parts)
  {
    Q_UNUSED(command)
    Q_UNUSED(parts)
  }

  void sendMessage(const QString &message);

private:
  void attemptConnection();
  void scheduleRetry();
  void handleHandshakeMessage(const QStringList &parts);

  QLocalSocket *m_socket;
  State m_state{State::Unconnected};
  QString m_socketName;
  QByteArray m_readBuffer;
  int m_retryCount{0};
  int m_maxAttempts;
  QTimer m_retryTimer;
  QElapsedTimer m_lastRetryLog;
  QString m_typeName;
};

} // namespace deskflow::gui::ipc
