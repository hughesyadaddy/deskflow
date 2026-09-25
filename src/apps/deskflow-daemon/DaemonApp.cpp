/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DaemonApp.h"

#include "arch/Arch.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/LogOutputters.h"
#include "common/ExitCodes.h"
#include "common/Settings.h"
#include "deskflow/ipc/DaemonIpcServer.h"

#if defined(Q_OS_WIN)
#include "arch/win32/ArchDaemonWindows.h"
#include "deskflow/Screen.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "platform/MSWindowsEventQueueBuffer.h"
#include "platform/MSWindowsWatchdog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#endif

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QSysInfo>

using namespace deskflow::core;

DaemonApp::DaemonApp(IEventQueue &events) : m_events(events)
{
  // do nothing
}

DaemonApp::~DaemonApp() = default;

void DaemonApp::saveLogLevel(const QString &logLevel) const
{
  LOG_DEBUG("log level changed: %s", logLevel.toUtf8().constData());
  CLOG->setFilter(logLevel.toUtf8().constData());
  Settings::setValue(Settings::Daemon::LogLevel, logLevel);
}

void DaemonApp::setConfigFile(const QString &configFile)
{
  LOG_DEBUG("config file updated: %s", configFile.toUtf8().constData());
  m_configFile = configFile;
  Settings::setValue(Settings::Daemon::ConfigFile, configFile);
}

void DaemonApp::applyWatchdogCommand() const
{
#if defined(Q_OS_WIN)
  if (m_configFile.isEmpty()) {
    LOG_ERR("cannot apply watchdog command: no config file set");
    return;
  }

  // QFileInfo::exists on a UNC path triggers SMB auth from this SYSTEM-context
  // process, leaking the machine NTLM hash to whoever controls the remote host.
  // Any local user can reach this via the IPC pipe, so reject remote paths up front.
  if (m_configFile.startsWith(QStringLiteral("\\\\")) || m_configFile.startsWith(QStringLiteral("//"))) {
    LOG_ERR("cannot apply watchdog command: remote config file paths are not allowed: %s", qPrintable(m_configFile));
    return;
  }

  if (!QFileInfo::exists(m_configFile)) {
    LOG_ERR("cannot apply watchdog command: config file does not exist: %s", qPrintable(m_configFile));
    return;
  }

  QSettings config(m_configFile, QSettings::IniFormat);

  // An explicit IPC start from the GUI is authoritative: the GUI has already decided the
  // daemon owns the core (it forces Service mode whenever the service is installed), so
  // the on-disk core/processMode is not consulted here. The Desktop-mode guard lives in
  // run(), on the unattended boot-time re-apply of the persisted daemon/configFile,
  // which is the only path where spawning would race a GUI-owned core.
  const auto coreMode = config.value(Settings::Core::CoreMode).toInt();

  QString modeArg;
  if (coreMode == Settings::CoreMode::Server) {
    modeArg = QStringLiteral("server");
  } else if (coreMode == Settings::CoreMode::Client) {
    modeArg = QStringLiteral("client");
  } else if (coreMode == Settings::CoreMode::Auto) {
    // Auto (native coordination mesh) runs server and client epochs in one
    // core process. The core stays at medium integrity for its whole life; the
    // secure/login desktop is reached by the VHID bridge, not by relaunching
    // the core elevated (see plan 2026-07-07).
    modeArg = QStringLiteral("auto");
  } else {
    LOG_ERR("cannot apply watchdog command: invalid core mode in config: %d", coreMode);
    return;
  }

  const auto corePath = QStringLiteral("%1/%2").arg(QCoreApplication::applicationDirPath(), kCoreBinName);
  const auto command = QStringLiteral("\"%1\" %2 --settings \"%3\"").arg(corePath, modeArg, m_configFile).toStdString();

  const auto uiAccessCore =
      config.value(Settings::Daemon::UiAccessCore, !Settings::isPortableMode()).toBool();

  LOG_DEBUG("applying watchdog command (uiAccessCore: %s)", uiAccessCore ? "yes" : "no");
  m_pWatchdog->setProcessConfig(command, uiAccessCore);
#else
  LOG_ERR("applying watchdog command not implemented on this platform");
#endif
}

void DaemonApp::clearWatchdogCommand()
{
  LOG_DEBUG("clearing watchdog command");

  // Only the running core stops. daemon/configFile stays persisted: every GUI stop
  // (including the auto-stop on an untrusted fingerprint) used to wipe it, after
  // which the daemon silently launched nothing at boot. Uninstall (clearSettings)
  // is the one path that forgets the config.
#if defined(Q_OS_WIN)
  m_pWatchdog->setProcessConfig("", false);
#else
  LOG_ERR("clearing watchdog command not implemented on this platform");
#endif
}

void DaemonApp::restartWatchdogProcess() const
{
#if defined(Q_OS_WIN)
  // Keyboard rescue (5x Esc / fleet rescue) reached a core with no GUI to
  // restart it; the core asked us instead. MSWindowsWatchdog::requestRestart
  // queues StartPending, and startProcess() replaces the running core.
  m_pWatchdog->requestRestart();
#else
  LOG_ERR("restarting watchdog process not implemented on this platform");
#endif
}

void DaemonApp::stopAllProcesses()
{
#if defined(Q_OS_WIN)
  QString seat;
  if (!m_configFile.isEmpty() && !m_configFile.startsWith(QStringLiteral("\\\\")) &&
      !m_configFile.startsWith(QStringLiteral("//"))) {
    seat = QSettings(m_configFile, QSettings::IniFormat).value(Settings::Core::ComputerName).toString();
  }
  if (seat.isEmpty()) {
    seat = QSysInfo::machineHostName();
  }
  // The WARNING line goes first, before anything is touched.
  LOG_WARN("[rescue] 10x Esc: stopping ALL Deskflow instances and services on %s", qPrintable(seat));

  // Clears the command (nothing is relaunched, daemon/configFile stays
  // persisted for the next `deskflow-ctl.ps1 start`), queues the stop of
  // the watchdog-owned core and terminates every deskflow-core.exe /
  // deskflow.exe under the install root in every session.
  m_pWatchdog->requestStopAll(QCoreApplication::applicationDirPath().toStdWString());

  // Clean self-stop: the event loop ends, mainLoop() returns s_exitSuccess,
  // and ArchDaemonWindows reports SERVICE_STOPPED with dwWin32ExitCode =
  // NO_ERROR. The SCM recovery actions deskflow-ctl.ps1 configures
  // (`sc failure ... restart/1000/restart/5000/restart/30000` +
  // `failureflag 1`) fire only when the process dies without reporting
  // SERVICE_STOPPED or reports it with a non-zero exit code, so this stop
  // stays down until `deskflow-ctl.ps1 start`.
  m_events.addEvent(Event(EventTypes::Quit));
#else
  LOG_ERR("stop-all not implemented on this platform");
#endif
}

void DaemonApp::clearSettings()
{
  LOG_INFO("clearing daemon settings");
  m_configFile.clear();
  Settings::setValue(Settings::Daemon::ConfigFile);
  Settings::setValue(Settings::Daemon::LogFile);
  Settings::setValue(Settings::Daemon::LogLevel);
}

void DaemonApp::connectIpcServer(const ipc::DaemonIpcServer *ipcServer) const
{
  // Use direct connection as this object is on it's own thread,
  // and so is on a different event loop to the main Qt loop.
  connect(ipcServer, &ipc::DaemonIpcServer::logLevelChanged, this, &DaemonApp::saveLogLevel, Qt::DirectConnection);
  connect(ipcServer, &ipc::DaemonIpcServer::configFileChanged, this, &DaemonApp::setConfigFile, Qt::DirectConnection);
  connect(
      ipcServer, &ipc::DaemonIpcServer::startProcessRequested, this, &DaemonApp::applyWatchdogCommand,
      Qt::DirectConnection
  );
  connect(
      ipcServer, &ipc::DaemonIpcServer::stopProcessRequested, this, &DaemonApp::clearWatchdogCommand,
      Qt::DirectConnection
  );
  connect(
      ipcServer, &ipc::DaemonIpcServer::clearSettingsRequested, this, &DaemonApp::clearSettings, Qt::DirectConnection
  );
  connect(
      ipcServer, &ipc::DaemonIpcServer::restartProcessRequested, this, &DaemonApp::restartWatchdogProcess,
      Qt::DirectConnection
  );
  connect(
      ipcServer, &ipc::DaemonIpcServer::stopAllRequested, this, &DaemonApp::stopAllProcesses, Qt::DirectConnection
  );
}

void DaemonApp::run(QThread &daemonThread)
{
  LOG_INFO("starting daemon");

  // Important: Move the daemon app to the daemon thread before creating any more Qt objects
  // owned by the daemon app, as they will be created on the daemon thread.
  moveToThread(&daemonThread);

  connect(&daemonThread, &QThread::started, this, [this, &daemonThread]() {
    LOG_DEBUG("daemon thread started");

    if (m_foreground) {
      LOG_DEBUG("running daemon in foreground");
      mainLoop();
    } else {
      LOG_DEBUG("running daemon in background (daemonizing)");
      ARCH->daemonize([this] { return daemonLoop(); });
    }

    daemonThread.quit();
    LOG_DEBUG("daemon thread finished");
  });

#if defined(Q_OS_WIN)
  m_pWatchdog = std::make_unique<MSWindowsWatchdog>(m_foreground, *m_pFileLogOutputter);

  if (const auto persistedConfig = Settings::value(Settings::Daemon::ConfigFile).toString();
      !persistedConfig.isEmpty()) {
    LOG_DEBUG("using last known config file: %s", persistedConfig.toUtf8().constData());
    m_configFile = persistedConfig;

    // Boot-time re-apply only: Desktop process mode means the GUI owns the core.
    // Spawning one here too put two cores on the machine: the second exits 5 and
    // the watchdog then backs off and relaunches forever. An explicit IPC start
    // (applyWatchdogCommand via startProcessRequested) is authoritative and skips this.
    // Remote paths are not opened here (SMB auth leak, see applyWatchdogCommand which rejects them).
    const auto isRemote =
        persistedConfig.startsWith(QStringLiteral("\\\\")) || persistedConfig.startsWith(QStringLiteral("//"));
    const auto bootMode = isRemote ? static_cast<int>(Settings::ProcessMode::Service)
                                   : QSettings(persistedConfig, QSettings::IniFormat)
                                         .value(Settings::Core::ProcessMode, Settings::ProcessMode::Service)
                                         .toInt();
    if (bootMode == Settings::ProcessMode::Desktop) {
      LOG_INFO(
          "config %s uses desktop process mode (GUI-owned core); daemon will not auto-spawn a core at boot",
          persistedConfig.toUtf8().constData()
      );
    } else {
      applyWatchdogCommand();
    }
  }
#endif

  LOG_DEBUG("starting daemon thread");
  daemonThread.start();
}

int DaemonApp::daemonLoop()
{
#if defined(Q_OS_WIN)
  // Runs the daemon through the Windows service controller, which controls the program lifecycle.
  return ArchDaemonWindows::runDaemon([this]() { return mainLoop(); });
#else
  return mainLoop();
#endif
}

int DaemonApp::mainLoop()
{
#if defined(Q_OS_WIN)
  if (m_pWatchdog == nullptr) {
    LOG_ERR("watchdog not initialized");
    return s_exitFailed;
  }
  ArchDaemonWindows::daemonRunning(true);
#endif

  try {
#if defined(Q_OS_WIN)
    // Install the platform event queue to handle service stop events.
    // This must be done on the same thread as the event loop, otherwise the service stop
    // request will not add the quit event to the event queue, and the service won't stop.
    m_events.adoptBuffer(new MSWindowsEventQueueBuffer(&m_events));

    LOG_DEBUG("starting watchdog threads");
    m_pWatchdog->startAsync();
#endif

    LOG_INFO("daemon is running");
    m_events.loop();
  } catch (std::exception &e) { // NOSONAR - Catching all exceptions
    LOG_CRIT("daemon error: %s", e.what());
  } catch (...) { // NOSONAR - Catching remaining exceptions
    LOG_CRIT("daemon unknown error");
  }

  LOG_INFO("daemon is stopping");

#if defined(Q_OS_WIN)
  try {
    LOG_DEBUG("stopping process watchdog");
    m_pWatchdog->stop();
  } catch (std::exception &e) { // NOSONAR - Catching all exceptions
    LOG_CRIT("daemon stop watchdog error: %s", e.what());
  } catch (...) { // NOSONAR - Catching remaining exceptions
    LOG_CRIT("daemon stop watchdog unknown error");
  }
  ArchDaemonWindows::daemonRunning(false);
#endif

  return s_exitSuccess;
}

QString DaemonApp::logFilename()
{
  return Settings::value(Settings::Daemon::LogFile).toString();
}

void DaemonApp::setForeground()
{
  m_foreground = true;
  showConsole();
}

void DaemonApp::initLogging()
{
#if defined(Q_OS_WIN)
  if (!m_foreground) {
    // Only use MS debug outputter when the process is daemonized, since stdout won't be accessible
    // in that case, but is accessible when running in the foreground.
    CLOG->insert(new MSWindowsDebugOutputter()); // NOSONAR - Adopted by `Log`
  }
#endif

  m_pFileLogOutputter = new FileLogOutputter(qPrintable(logFilename())); // NOSONAR - Adopted by `Log`
  CLOG->insert(m_pFileLogOutputter);
}

void DaemonApp::showConsole()
{
#if defined(Q_OS_WIN)
  // The daemon bin is compiled using the Win32 subsystem which works best for Windows services,
  // so when running as a foreground process we need to allocate a console (or we won't see output).
  // It is important to do this inside the arg check loop so that we can attach console ahead
  // of log output generated by handling other args.
  AllocConsole();
  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
#endif
}
