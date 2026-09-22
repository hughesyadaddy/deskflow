/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreProcess.h"

#include "common/ExitCodes.h"
#include "gui/ipc/CoreIpcClient.h"
#include "gui/ipc/DaemonIpcClient.h"

#if defined(Q_OS_MACOS)
#include "OSXHelpers.h"
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <signal.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaEnum>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>

namespace deskflow::gui {

const int kRetryDelay = 1000;
const auto kLineSplitRegex = QRegularExpression("\r|\n|\r\n");

#ifdef Q_OS_MACOS
//! launchd label of the user-domain core agent installed by the macOS installer.
const auto kLaunchdCoreLabel = QStringLiteral("io.github.hughesyadaddy.deskflow-core");

QString launchdCoreTarget()
{
  return QStringLiteral("gui/%1/%2").arg(getuid()).arg(kLaunchdCoreLabel);
}
#endif

QString CoreProcess::processModeToString(const Settings::ProcessMode mode)
{
  return QVariant::fromValue(mode).toString().toLower();
}

QString CoreProcess::processStateToString(const CoreProcess::ProcessState state)
{
  return QVariant::fromValue(state).toString().toLower();
}

/**
 * @brief Wraps options that contain spaces in quotes
 *
 * Useful to handle things like paths with spaces (e.g. "C:\Program Files").
 *
 * Can also be used to create a representation of a command that can be pasted
 * into a terminal.
 */
QString CoreProcess::makeQuotedArgs(const QString &app, const QStringList &args)
{
  QStringList command = {app};
  command.append(args);

  static const auto quote = QStringLiteral("\"");
  static const auto space = QStringLiteral(" ");
  QStringList quoted;
  for (const auto &item : std::as_const(command)) {
    auto temp = item.simplified();
    if (const auto wrapped = (temp.startsWith(quote) && temp.endsWith(quote)); temp.contains(space) && !wrapped) {
      temp = QStringLiteral("%1%2%1").arg(quote, temp);
    }
    quoted.append(temp);
  }

  return quoted.join(space);
}

/**
 * @brief If IPv6, ensures the IP is surround in square brackets.
 */
QString CoreProcess::wrapIpv6(const QString &address)
{
  static const auto colon = QStringLiteral(":");
  static const auto openBracket = QStringLiteral("[");
  static const auto closeBracket = QStringLiteral("]");

  if (!address.contains(colon) || address.isEmpty()) {
    return address;
  }

  QString wrapped = address;

  if (!address.startsWith(openBracket)) {
    wrapped.prepend(openBracket);
  }

  if (!address.endsWith(closeBracket)) {
    wrapped.append(closeBracket);
  }

  return wrapped;
}

//
// CoreProcess
//

CoreProcess::CoreProcess(const ServerConfig &serverConfig, const QString &appPathOverride)
    : m_serverConfig(serverConfig),
      m_daemonIpcClient{new ipc::DaemonIpcClient(this)}
{
  if (appPathOverride.isEmpty()) {
    m_appPath = QStringLiteral("%1/%2").arg(QCoreApplication::applicationDirPath(), kCoreBinName);
    if (!QFile::exists(m_appPath)) {
      qFatal("core server binary does not exist");
      return;
    }
  } else {
    m_appPath = appPathOverride;
  }

  m_retryTimer.setSingleShot(true);
  m_restartTimer.setSingleShot(true);
  connect(&m_restartTimer, &QTimer::timeout, this, &CoreProcess::doRestart);

  connect(m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, &CoreProcess::daemonIpcClientConnected);
  connect(
      m_daemonIpcClient, &ipc::DaemonIpcClient::connectionFailed, this, &CoreProcess::daemonIpcClientConnectionFailed
  );
  connect(m_daemonIpcClient, &ipc::DaemonIpcClient::logPathReceived, this, &CoreProcess::setupDaemonLogTail);

  connect(&m_retryTimer, &QTimer::timeout, this, [this] {
    if (m_processState == ProcessState::RetryPending) {
      start();
    } else {
      qDebug("retry cancelled, process state is not retry pending");
    }
  });
}

void CoreProcess::onProcessReadyReadStandardOutput()
{
  if (m_process) {
    handleLogLines(m_process->readAllStandardOutput());
  }
}

void CoreProcess::onProcessReadyReadStandardError()
{
  if (m_process) {
    handleLogLines(m_process->readAllStandardError());
  }
}

void CoreProcess::daemonIpcClientConnected()
{
  applyLogLevel();
  m_daemonIpcClient->requestLogPath();
}

void CoreProcess::releaseProcess()
{
  if (!m_process) {
    return;
  }

  // Detach before deleting so a late signal from the old object can never be
  // attributed to a newer process.
  disconnect(m_process, nullptr, this, nullptr);
  if (m_process->state() != QProcess::NotRunning) {
    qWarning("releasing core process that is still running, killing it");
    m_process->kill();
  }
  m_process->deleteLater();
  m_process = nullptr;
}

void CoreProcess::scheduleRetry(int delayMs)
{
  setProcessState(ProcessState::RetryPending);
  m_retryTimer.start(delayMs);
}

void CoreProcess::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
  using enum ProcessState;
  setConnectionState(ConnectionState::Disconnected);

  if (m_retryTimer.isActive()) {
    m_retryTimer.stop();
  }

  const auto wasStarted = m_processState == Started;
  releaseProcess();

  // Another core already owns this machine (the service's core or a second GUI).
  if (exitCode == s_exitDuplicate && exitStatus == QProcess::NormalExit) {
    m_consecutiveCrashes = 0;
    // Never stop or kill the other core and never retry: retrying would just re-collide,
    // and stopping it via IPC makes its supervisor respawn it, which ping-pongs forever.
    qWarning("another core owns this machine (exit code %d), leaving it running and not retrying", exitCode);
    setProcessState(Stopped);
    return;
  }

  if (exitStatus == QProcess::CrashExit) {
    if (!wasStarted) {
      // Stopping/Stopped: we terminated/killed it ourselves; a signal exit is the expected outcome.
      qDebug("desktop process ended by our stop request");
      setProcessState(Stopped);
      return;
    }

    m_consecutiveCrashes++;
    if (m_consecutiveCrashes > kMaxCrashRetries) {
      qCritical("core process crashed %d times in a row, giving up; use Start to try again", m_consecutiveCrashes);
      setProcessState(Stopped);
      Q_EMIT error(Error::CrashLoop);
      return;
    }

    const auto delay = std::min(kCrashRetryBaseDelayMs << (m_consecutiveCrashes - 1), kMaxCrashRetryDelayMs);
    qCritical(
        "core process crashed (exit code %d), retry %d of %d in %d ms", //
        exitCode, m_consecutiveCrashes, kMaxCrashRetries, delay
    );
    scheduleRetry(delay);
    return;
  }

  if (exitCode != s_exitSuccess) {
    qWarning("desktop process exited with code: %d", exitCode);
    setProcessState(Stopped);
    return;
  }

  qDebug("desktop process exited normally");

  if (wasStarted) {
    qDebug("desktop process was running, retrying in %d ms", kRetryDelay);
    scheduleRetry(kRetryDelay);
  } else {
    setProcessState(Stopped);
  }
}

bool CoreProcess::spawnCoreProcess(QProcess *process, const QString &program, const QStringList &args)
{
#ifdef Q_OS_LINUX
  process->setChildProcessModifier([] {
    // the core process becomes orphaned when the gui process exits abruptly (e.g. with kill -9),
    // so ensure the os also kills the core when that happens to the gui.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
  });
#endif

  process->start(program, args);
  return process->waitForStarted();
}

bool CoreProcess::hasExternalSupervisor() const
{
  // macOS with the fleet core agent installed: launchd is the only thing that may
  // spawn a core. The old `launchctl print` probe timed out at login storms and the
  // GUI then spawned its own core, leaving launchd's agent looping on exit 5; a
  // file stat cannot time out. Without the agent (dev machines) the GUI spawns.
  // This stays plist-based on purpose (unlike macLaunchdOwnsGui, which is
  // env-only): the question here is "does launchd own the CORE on this box",
  // which is true for every GUI copy, launchd's or not -- a Login Item copy
  // must not spawn a second core just because launchd did not start *it*.
#ifdef Q_OS_MACOS
  return macLaunchdOwnsCore();
#else
  return false;
#endif
}

void CoreProcess::kickstartExternalCore()
{
#ifdef Q_OS_MACOS
  const auto target = launchdCoreTarget();
  qInfo("restarting launchd-managed core: launchctl kickstart -k %s", qPrintable(target));
  auto *launchctl = new QProcess(this);
  connect(launchctl, &QProcess::finished, this, [this, launchctl](int exitCode, QProcess::ExitStatus status) {
    launchctl->deleteLater();
    if (status != QProcess::NormalExit || exitCode != 0) {
      qWarning("launchctl kickstart failed with exit code %d", exitCode);
      kickstartFailed();
    }
  });
  connect(launchctl, &QProcess::errorOccurred, this, [this, launchctl](QProcess::ProcessError) {
    if (launchctl->state() == QProcess::NotRunning) {
      qWarning("launchctl kickstart could not run: %s", qPrintable(launchctl->errorString()));
      launchctl->deleteLater();
      kickstartFailed();
    }
  });
  launchctl->start(QStringLiteral("/bin/launchctl"), {QStringLiteral("kickstart"), QStringLiteral("-k"), target});
#endif
}

bool CoreProcess::isWindowsServiceInstalled() const
{
#ifdef Q_OS_WIN
  SC_HANDLE mgr = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!mgr) {
    return false;
  }
  SC_HANDLE service = OpenService(mgr, QString::fromLatin1(kAppName).toStdWString().c_str(), SERVICE_QUERY_STATUS);
  const bool installed = service != nullptr;
  if (service) {
    CloseServiceHandle(service);
  }
  CloseServiceHandle(mgr);
  return installed;
#else
  return false;
#endif
}

void CoreProcess::applyLogLevel()
{
  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>();
  if (processMode == ProcessMode::Service) {
    qDebug() << "setting daemon log level:" << Settings::logLevelText();
    m_daemonIpcClient->sendLogLevel(Settings::logLevelText());
  }
}

void CoreProcess::startForegroundProcess(const QStringList &args)
{
  using enum ProcessState;

  if (m_processState != Starting) {
    qFatal("core process must be in starting state");
  }

  // only make quoted args for printing the command for convenience; so that the
  // core command can be easily copy/pasted to the terminal for testing.
  const auto quoted = makeQuotedArgs(m_appPath, args);
  qInfo("running command: %s", qPrintable(quoted));

  if (spawnCoreProcess(m_process, m_appPath, args)) {
    setProcessState(Started);
  } else {
    releaseProcess();
    setProcessState(Stopped);
    Q_EMIT error(Error::StartFailed);
  }
}

void CoreProcess::startProcessFromDaemon()
{
  if (m_processState != ProcessState::Starting) {
    qFatal("core process must be in starting state");
  }

  const auto configFile = Settings::settingsFile();
  qInfo("sending start to daemon (config file: %s)", qPrintable(configFile));

  auto sendStart = [this, configFile] {
    m_daemonIpcClient->sendConfigFile(configFile);
    m_daemonIpcClient->sendStartProcess();
    setProcessState(ProcessState::Started);
  };

  if (m_daemonIpcClient->isConnected()) {
    sendStart();
  } else {
    connect(
        m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, sendStart,
        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
    );
    m_daemonIpcClient->connectToServer();
  }
}

void CoreProcess::stopForegroundProcess()
{
  if (m_processState != ProcessState::Stopping) {
    qFatal("core process must be in stopping state");
  }

  if (!m_process) {
    qWarning("process not set, nothing to stop");
    setProcessState(ProcessState::Stopped);
    return;
  }

  qInfo("stopping core desktop process");

  if (m_process->state() == QProcess::ProcessState::Running) {
    // SIGTERM first so the core can tear down its IPC server and virtual devices; only
    // escalate to SIGKILL when it ignores us. waitForFinished delivers finished() to
    // onProcessFinished synchronously, which sees Stopping and settles on Stopped.
    qDebug("process is running, terminating (grace %d ms)", kStopGraceMs);
    m_process->terminate();
    if (!m_process->waitForFinished(kStopGraceMs)) {
      qWarning("core did not exit within %d ms after SIGTERM, killing", kStopGraceMs);
      m_process->kill();
      m_process->waitForFinished(1000);
    }
  } else {
    qDebug("process is not running, skipping terminate");
  }

  releaseProcess();
  setProcessState(ProcessState::Stopped);
}

void CoreProcess::stopProcessFromDaemon()
{
  if (m_processState != ProcessState::Stopping) {
    qFatal("core process must be in stopping state");
  }

  auto sendStop = [this] {
    m_daemonIpcClient->sendStopProcess();
    setProcessState(ProcessState::Stopped);
  };

  if (m_daemonIpcClient->isConnected()) {
    sendStop();
  } else {
    connect(
        m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, sendStop,
        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
    );
    m_daemonIpcClient->connectToServer();
  }
}

void CoreProcess::handleLogLines(const QString &text)
{
  const auto lines = text.split(kLineSplitRegex);
  for (const auto &line : lines) {
    if (line.isEmpty()) {
      continue;
    }

#if defined(Q_OS_MACOS)
    // HACK: macOS 10.13.4+ spamming error lines in logs making them
    // impossible to read and debug; giving users a red herring.
    if (line.contains("calling TIS/TSM in non-main thread environment")) {
      continue;
    }

    // the core process is not allowed to show the permission prompt
    // (called "notification permission") and the notification log line is emitted from
    // deep inside cocoa code in the core binary to stdout, so it can't be sent over
    // ipc from the core to the gui and instead the gui has to parse the core output.
    static const QString needle = "OSX Notification: ";
    if (line.contains(needle) && line.contains('|')) {
      const int delimiterPosition = line.indexOf('|');
      const int start = line.indexOf(needle);
      const QString title = line.mid(start + needle.length(), delimiterPosition - start - needle.length());
      const QString body = line.mid(delimiterPosition + 1, line.length() - delimiterPosition);
      if (!showOSXNotification(title, body)) {
        qDebug("osx notification was not shown");
      }
    }
#endif

    Q_EMIT logLine(line);
  }
}

void CoreProcess::start(std::optional<ProcessMode> processModeOption)
{
  if (m_processState == ProcessState::Started) {
    qCritical("core process already started");
    return;
  }

  if (m_mode == Settings::CoreMode::None) {
    qFatal("set core mode before starting");
    return;
  }

  QMutexLocker locker(&m_processMutex);

  // A retry keeps the crash count; anything else (user, restart) is a fresh attempt.
  if (m_processState != ProcessState::RetryPending) {
    m_consecutiveCrashes = 0;
  }
  if (m_retryTimer.isActive()) {
    m_retryTimer.stop();
  }

  const auto currentMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  auto processMode = processModeOption.value_or(currentMode);
  const auto coreMode = QVariant::fromValue(m_mode).toString().toLower();

  // On Windows the service's watchdog owns the core; a second Desktop-mode core would only
  // collide with it (exit 5) or fight it for the virtual devices.
  if (processMode == ProcessMode::Desktop && isWindowsServiceInstalled()) {
    if (!m_serviceModeForcedLogged) {
      qWarning("the %s service is installed, refusing desktop mode and using service mode", kAppName);
      m_serviceModeForcedLogged = true;
    }
    processMode = ProcessMode::Service;
    // The daemon re-reads the on-disk settings file we hand it (startProcessFromDaemon); if
    // that still says Desktop, its boot-time guard refuses to spawn and the GUI reports
    // Started with no core anywhere. Persist the forced mode before sending the start.
    if (currentMode != ProcessMode::Service) {
      Settings::setValue(Settings::Core::ProcessMode, ProcessMode::Service);
      Settings::save(false); // sync only; no serverSettingsChanged cascade mid-start
    }
  }

  // An externally supervised core (launchd on macOS) is attached to via IPC only.
  m_externallySupervised = processMode == ProcessMode::Desktop && hasExternalSupervisor();

  qInfo().noquote() << QString("starting %1 process (%2 mode%3)")
                           .arg(
                               coreMode, processModeToString(processMode),
                               m_externallySupervised ? QStringLiteral(", externally supervised") : QString()
                           );

  setProcessState(ProcessState::Starting);

#ifdef Q_OS_MACOS
  requestOSXNotificationPermission();
#endif

  setConnectionState(ConnectionState::Connecting);

  if (processMode == ProcessMode::Desktop && !m_externallySupervised) {
    releaseProcess();
    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, &CoreProcess::onProcessFinished);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &CoreProcess::onProcessReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &CoreProcess::onProcessReadyReadStandardError);
  }

  QStringList args = {coreMode};

  // Auto mode runs server epochs too, so it needs the layout persisted.
  if (m_mode == Settings::CoreMode::Server || m_mode == Settings::CoreMode::Auto) {
    const auto [hasNeededPermissions, configFilename] = persistServerConfig();
    if (configFilename.isEmpty()) {
      qFatal("config file name empty for server args");
      return;
    }
    if (!hasNeededPermissions) {
      setProcessState(ProcessState::Stopped);
      setConnectionState(ConnectionState::Disconnected);
      Q_EMIT error(Error::StartFailed);
      return;
    }
    qInfo("core config file: %s", qPrintable(configFilename));
  }

  qDebug().noquote() << "log level:" << Settings::logLevelText();

  if (Settings::value(Settings::Log::ToFile).toBool()) {
    const auto logFile = Settings::value(Settings::Log::File).toString();
    QDir(QFileInfo(logFile).absolutePath()).mkpath(".");
    qInfo().noquote() << "log file:" << logFile;
  }

  // Wired before the start calls so it catches Started from both sync (desktop) and async (service) paths.
  connect(
      this, &CoreProcess::processStateChanged, this,
      [this](ProcessState state) {
        if (state != ProcessState::Started) {
          return;
        }

        // Delay briefly to give the core process time to start its IPC server.
        QTimer::singleShot(kRetryDelay, this, [this] {
          if (m_processState != ProcessState::Started) {
            return;
          }

          releaseCoreIpcClient();
          m_coreIpcClient = new ipc::CoreIpcClient(this);
          connect(m_coreIpcClient, &ipc::CoreIpcClient::commandReceived, this, &CoreProcess::onCoreIpcMessageReceived);
          connect(m_coreIpcClient, &ipc::CoreIpcClient::connected, this, [this] {
            qDebug("connected to core ipc server");
            m_consecutiveCrashes = 0;
          });
          // The client retries a fresh connect forever, so these only fire once a
          // connection is lost (kickstart, core crash, core stop): re-attach while the
          // core is still meant to be running.
          connect(m_coreIpcClient, &ipc::CoreIpcClient::connectionFailed, this, [this] {
            qWarning("core ipc connection lost, re-attaching");
            scheduleCoreIpcReattach();
          });
          connect(m_coreIpcClient, &ipc::CoreIpcClient::serverShutdown, this, [this] {
            qDebug("core ipc server shut down cleanly, re-attaching");
            scheduleCoreIpcReattach();
          });

          m_coreIpcClient->connectToServer();
        });
      },
      static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
  );

  if (m_externallySupervised) {
    qInfo("core is supervised by launchd, attaching via ipc without spawning");
    setProcessState(ProcessState::Started);
  } else if (processMode == ProcessMode::Desktop) {
    startForegroundProcess(args);
  } else if (processMode == ProcessMode::Service) {
    startProcessFromDaemon();
  }

  m_lastProcessMode = processMode;
}

void CoreProcess::stop(std::optional<ProcessMode> processModeOption)
{
  QMutexLocker locker(&m_processMutex);

  const auto currentMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  const auto processMode = processModeOption.value_or(currentMode);

  qInfo("stopping core process (%s mode)", qPrintable(processModeToString(processMode)));

  m_consecutiveCrashes = 0;
  if (m_retryTimer.isActive()) {
    m_retryTimer.stop();
  }

  releaseCoreIpcClient();

  if (m_processState == ProcessState::Starting) {
    qDebug("core process is starting, cancelling");
    setProcessState(ProcessState::Stopped);
  } else if (m_processState != ProcessState::Stopped) {
    setProcessState(ProcessState::Stopping);

    if (m_externallySupervised) {
      qInfo("core is supervised by launchd, detaching and leaving it running");
      setProcessState(ProcessState::Stopped);
    } else if (processMode == ProcessMode::Service) {
      stopProcessFromDaemon();
    } else if (processMode == ProcessMode::Desktop) {
      stopForegroundProcess();
    }

  } else {
    qWarning("core process already stopped");
  }

  setConnectionState(ConnectionState::Disconnected);
}

void CoreProcess::reloadServerConfig()
{
  if (!isStarted()) {
    return;
  }

  const auto mode = m_mode;
  if (mode != Settings::CoreMode::Server && mode != Settings::CoreMode::Auto) {
    restart();
    return;
  }

  const auto [hasNeededPermissions, configFilename] = persistServerConfig();
  if (!hasNeededPermissions || configFilename.isEmpty()) {
    qWarning("reloadServerConfig: could not persist server layout");
    restart();
    return;
  }

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
  if (m_process != nullptr && m_process->state() == QProcess::Running) {
    qDebug("signaling deskflow-core to reload configuration (SIGHUP)");
    ::kill(static_cast<pid_t>(m_process->processId()), SIGHUP);
    return;
  }
#endif

  restart();
}

void CoreProcess::restart()
{
  // Rate-limit: settings churn, the 5x Esc rescue and reloadServerConfig can all fire
  // restart() in quick succession; one restart per window is enough, later calls coalesce.
  if (m_lastRestart.isValid() && m_lastRestart.elapsed() < kRestartMinIntervalMs) {
    const auto remaining = static_cast<int>(kRestartMinIntervalMs - m_lastRestart.elapsed());
    if (!m_restartTimer.isActive()) {
      qDebug("restart requested within %d ms of the last one, coalescing (in %d ms)", kRestartMinIntervalMs, remaining);
      m_restartTimer.start(std::max(remaining, 1));
    }
    return;
  }

  doRestart();
}

void CoreProcess::doRestart()
{
  qDebug("restarting core process");
  m_lastRestart.restart();

  if (m_externallySupervised && m_processState == ProcessState::Started) {
    // launchd owns the process: one kickstart, and the IPC client re-attaches with backoff.
    setConnectionState(ConnectionState::Connecting);
    kickstartExternalCore();
    return;
  }

  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();

  if (m_lastProcessMode != std::nullopt && m_lastProcessMode != processMode) {
    const auto debugMessage =
        QStringLiteral("process mode changed to %1, stopping %2 process")
            .arg(processModeToString(processMode), processModeToString(m_lastProcessMode.value()));
    qDebug().noquote() << debugMessage;
    stop(m_lastProcessMode);
  } else {
    // in service mode: though there is technically no need to stop the service
    // before restarting it, it does make for cleaner process state tracking,
    // especially if something goes wrong with starting the service.
    stop();
  }

  start();
}

void CoreProcess::kickstartFailed()
{
  // launchd could not bounce the core: there is nothing to attach to, say so.
  releaseCoreIpcClient();
  setProcessState(ProcessState::Stopped);
  setConnectionState(ConnectionState::Disconnected);
  Q_EMIT error(Error::StartFailed);
}

void CoreProcess::releaseCoreIpcClient()
{
  if (!m_coreIpcClient) {
    return;
  }
  disconnect(m_coreIpcClient, nullptr, this, nullptr);
  m_coreIpcClient->disconnectFromServer();
  m_coreIpcClient->deleteLater();
  m_coreIpcClient = nullptr;
}

void CoreProcess::scheduleCoreIpcReattach()
{
  QTimer::singleShot(kRetryDelay, this, [this] {
    if (m_processState == ProcessState::Started && m_coreIpcClient && !m_coreIpcClient->isConnected()) {
      m_coreIpcClient->connectToServer();
    }
  });
}

void CoreProcess::cleanup()
{
  qInfo("cleaning up core process");

  const auto isDesktop = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>() == ProcessMode::Desktop;
  const auto isRunning = m_processState == ProcessState::Started;
  if (isDesktop && isRunning) {
    stop();
  }
}

QPair<bool, QString> CoreProcess::persistServerConfig() const
{
  if (Settings::value(Settings::Server::ExternalConfig).toBool()) {
    return {Settings::isServerConfigFileReadable(), Settings::value(Settings::Server::ExternalConfigFile).toString()};
  }

  const auto configFilePath = Settings::defaultValue(Settings::Server::ExternalConfigFile).toString();
  QFile configFile(configFilePath);
  if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    qWarning() << "failed to open core config file for write:" << configFilePath;
    return {false, configFile.fileName()};
  }

  m_serverConfig.save(configFile);
  configFile.close();
  return {Settings::isServerConfigFileReadable(), configFile.fileName()};
}

void CoreProcess::setConnectionState(ConnectionState state)
{
  if (m_connectionState == state) {
    return;
  }

  m_connectionState = state;
  Q_EMIT connectionStateChanged(state);
}

void CoreProcess::setProcessState(ProcessState state)
{
  if (m_processState == state) {
    return;
  }

  qDebug(
      "core process state changed: %s -> %s", //
      qPrintable(processStateToString(m_processState)), qPrintable(processStateToString(state))
  );
  m_processState = state;
  Q_EMIT processStateChanged(state);
}

void CoreProcess::onCoreIpcMessageReceived(const QString &command, const QString &args)
{
  if (command == "restartCore") {
    qInfo("core requested restart (keyboard rescue 5x Esc)");
    restart();
    return;
  }
  if (command == "connectionState") {
    const auto metaEnum = QMetaEnum::fromType<ConnectionState>();
    bool ok = false;
    const auto state = static_cast<ConnectionState>(metaEnum.keyToValue(args.toUtf8().constData(), &ok));
    if (!ok) {
      qWarning("core ipc got unknown connection state: %s", args.toUtf8().constData());
      return;
    }
    setConnectionState(state);
  } else if (command == "connectedClients") {
    const auto clients = args.isEmpty() ? QStringList() : args.split(",");
    Q_EMIT connectedClientsChanged(clients);
  } else if (command == "secureSocket") {
    Q_EMIT secureSocket(true);
    if (args != m_secureSocketVersion) {
      m_secureSocketVersion = args;
      Q_EMIT securityLevelChanged(args);
    }
  } else if (command == "unrecognisedClient") {
    Q_EMIT unrecognisedClient(args);
  } else if (command == "connectionRefused") {
    const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionRefusal>();
    bool ok = false;
    const auto reason =
        static_cast<deskflow::core::ConnectionRefusal>(metaEnum.keyToValue(args.toUtf8().constData(), &ok));
    if (ok) {
      Q_EMIT connectionRefused(reason);
    } else {
      qWarning("core ipc got unknown connection refusal: %s", args.toUtf8().constData());
    }
  } else if (command == "retryIn") {
    Q_EMIT retryIn(args.toInt());
  } else if (command == "peerFingerprint") {
    Q_EMIT peerFingerprint(args);
  } else if (command == "missingKeyboardLayouts") {
    Q_EMIT missingKeyboardLayouts(args);
  }
}

bool CoreProcess::checkSecureSocket(const QString &line)
{
  static const QString tlsCheckString = "network encryption protocol: ";
  const auto index = line.indexOf(tlsCheckString, 0, Qt::CaseInsensitive);
  if (index == -1) {
    return false;
  }

  Q_EMIT secureSocket(true);
  if (const auto ssv = line.mid(index + tlsCheckString.size()); ssv != m_secureSocketVersion) {
    m_secureSocketVersion = ssv;
    Q_EMIT securityLevelChanged(ssv);
  }

  return true;
}

QString CoreProcess::correctedAddress(const QString &address) const
{
  return wrapIpv6(address.simplified());
}

void CoreProcess::setupDaemonLogTail(const QString &logPath)
{
  qDebug() << "daemon log path:" << logPath;

  if (QFileInfo logFile(logPath); !logFile.isFile()) {
    auto file = QFile(logPath);
    if (!file.open(QFile::ReadWrite)) {
      qCritical() << "daemon log path file can not be written:" << logPath;
      return;
    }
    file.write(""); // Create an empty file
  }

  if (m_daemonFileTail) {
    m_daemonFileTail->setWatchedFile(logPath);
  } else {
    m_daemonFileTail = new FileTail(logPath, this);
    connect(m_daemonFileTail, &FileTail::newLine, this, &CoreProcess::handleLogLines);
  }
}

void CoreProcess::clearSettings()
{
  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  if (processMode == ProcessMode::Desktop) {
    qDebug("no core settings to clear in desktop mode");
    return;
  }

  if (processMode != ProcessMode::Service) {
    qFatal("invalid process mode");
  }

  qInfo("clearing core settings through daemon");
  m_daemonIpcClient->sendClearSettings();
}

void CoreProcess::retryDaemon()
{
  m_daemonIpcClient->connectToServer();
}

} // namespace deskflow::gui
