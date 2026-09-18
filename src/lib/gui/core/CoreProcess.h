/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Enums.h"
#include "common/Settings.h"
#include "gui/FileTail.h"
#include "gui/config/ServerConfig.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace deskflow::gui {

namespace ipc {
class CoreIpcClient;
class DaemonIpcClient;
} // namespace ipc

class CoreProcess : public QObject
{
  using ConnectionState = deskflow::core::ConnectionState;
  using ProcessMode = Settings::ProcessMode;
  using ProcessState = deskflow::core::ProcessState;
  Q_OBJECT

public:
  enum class Error
  {
    AddressMissing,
    StartFailed,
    //! The core crashed @c kMaxCrashRetries times in a row; supervision gave up.
    CrashLoop
  };

  //! Retries scheduled after consecutive crashes before giving up (state -> Stopped).
  static constexpr int kMaxCrashRetries = 5;
  //! First crash retry delay; doubles per consecutive crash up to @c kMaxCrashRetryDelayMs.
  static constexpr int kCrashRetryBaseDelayMs = 1000;
  static constexpr int kMaxCrashRetryDelayMs = 30000;
  //! Minimum spacing between two effective restarts; calls inside the window coalesce into one.
  static constexpr int kRestartMinIntervalMs = 5000;
  //! Grace period between SIGTERM and SIGKILL when the GUI owns the core process.
  static constexpr int kStopGraceMs = 5000;

  /**
   * @param appPathOverride When non-empty, used as the core binary path and its existence is
   *                        not checked (tests fake the process boundary and never spawn it).
   */
  explicit CoreProcess(const ServerConfig &serverConfig, const QString &appPathOverride = {});

  void start(std::optional<ProcessMode> processMode = std::nullopt);
  void stop(std::optional<ProcessMode> processMode = std::nullopt);
  void restart();
  //! Persist server layout and signal deskflow-core to reload (SIGHUP on Unix).
  void reloadServerConfig();
  void cleanup();
  void applyLogLevel();
  void clearSettings();
  void retryDaemon();

  // getters
  Settings::CoreMode mode() const
  {
    return m_mode;
  }
  QString secureSocketVersion() const
  {
    return m_secureSocketVersion;
  }
  bool isStarted() const
  {
    return m_processState == ProcessState::Started;
  }
  ProcessState processState() const
  {
    return m_processState;
  }
  ConnectionState connectionState() const
  {
    return m_connectionState;
  }
  //! Consecutive crash-exits seen since the last healthy IPC connection or user-initiated stop.
  int consecutiveCrashes() const
  {
    return m_consecutiveCrashes;
  }
  //! Delay of the pending retry in ms, or -1 when no retry is scheduled.
  int pendingRetryDelayMs() const
  {
    return m_retryTimer.isActive() ? m_retryTimer.interval() : -1;
  }
  //! True when a coalesced restart is waiting for the rate-limit window to expire.
  bool hasPendingRestart() const
  {
    return m_restartTimer.isActive();
  }
  //! True when the core is supervised outside the GUI (launchd on macOS); the GUI only attaches via IPC.
  bool isExternallySupervised() const
  {
    return m_externallySupervised;
  }

  // setters
  void setAddress(const QString &address)
  {
    m_address = correctedAddress(address);
  }
  void setMode(Settings::CoreMode mode)
  {
    m_mode = mode;
  }

Q_SIGNALS:
  void error(deskflow::gui::CoreProcess::Error error);
  void logLine(const QString &line);
  void connectionStateChanged(deskflow::core::ConnectionState state);
  void processStateChanged(deskflow::core::ProcessState state);
  void secureSocket(bool enabled);
  void daemonIpcClientConnectionFailed();
  void connectedClientsChanged(const QStringList &clients);
  void securityLevelChanged(QString securityLevel);
  void unrecognisedClient(const QString &clientName);
  void connectionRefused(deskflow::core::ConnectionRefusal reason);
  void retryIn(int seconds);
  void peerFingerprint(const QString &fingerprint);
  void missingKeyboardLayouts(const QString &layouts);

protected Q_SLOTS:
  void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

protected:
  // Process-boundary seams. Tests override these so no real core is ever spawned or signalled.

  //! Start @p process; return false when it could not be started.
  virtual bool spawnCoreProcess(QProcess *process, const QString &program, const QStringList &args);
  //! True when an external supervisor owns the core (always on macOS: launchd), so the GUI never spawns one.
  virtual bool hasExternalSupervisor() const;
  //! Ask the external supervisor to restart the core (launchctl kickstart -k); never blocks.
  virtual void kickstartExternalCore();
  //! True when the Deskflow Windows service is installed (always false off Windows).
  virtual bool isWindowsServiceInstalled() const;

private Q_SLOTS:
  void onProcessReadyReadStandardOutput();
  void onProcessReadyReadStandardError();
  void onCoreIpcMessageReceived(const QString &command, const QString &args);
  void daemonIpcClientConnected();

private:
  void startForegroundProcess(const QStringList &args);
  void startProcessFromDaemon();
  void stopForegroundProcess();
  void stopProcessFromDaemon();
  void releaseProcess();
  void releaseCoreIpcClient();
  void scheduleCoreIpcReattach();
  void scheduleRetry(int delayMs);
  void doRestart();
  QPair<bool, QString> persistServerConfig() const;
  void setConnectionState(ConnectionState state);
  void setProcessState(ProcessState state);
  bool checkSecureSocket(const QString &line);
  void handleLogLines(const QString &text);
  QString correctedAddress(const QString &address) const;
  void setupDaemonLogTail(const QString &logPath);
  static QString makeQuotedArgs(const QString &app, const QStringList &args);
  static QString processModeToString(const Settings::ProcessMode mode);
  static QString processStateToString(const CoreProcess::ProcessState state);
  static QString wrapIpv6(const QString &address);

  const ServerConfig &m_serverConfig;
  QString m_address;
  ProcessState m_processState = ProcessState::Stopped;
  ConnectionState m_connectionState = ConnectionState::Disconnected;
  Settings::CoreMode m_mode = Settings::CoreMode::None;
  QMutex m_processMutex;
  QString m_secureSocketVersion;
  std::optional<ProcessMode> m_lastProcessMode = std::nullopt;
  QTimer m_retryTimer;
  QTimer m_restartTimer;
  QElapsedTimer m_lastRestart;
  int m_consecutiveCrashes = 0;
  bool m_externallySupervised = false;
  bool m_serviceModeForcedLogged = false;
  deskflow::gui::ipc::CoreIpcClient *m_coreIpcClient = nullptr;
  deskflow::gui::ipc::DaemonIpcClient *m_daemonIpcClient = nullptr;
  FileTail *m_daemonFileTail = nullptr;
  QProcess *m_process = nullptr;
  QString m_appPath;
};

} // namespace deskflow::gui
