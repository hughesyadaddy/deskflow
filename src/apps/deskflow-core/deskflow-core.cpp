/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2025 - 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AutoModeRunner.h"
#include "HealthReport.h"
#if SYSAPI_WIN32
#include "deskflow/win32/AppUtilWindows.h"
#endif
#include "CoreArgParser.h"
#include "PermissionCheck.h"

#if defined(Q_OS_WIN)
// WIN32_LEAN_AND_MEAN: plain windows.h drags in the legacy winsock.h, which
// then collides with the winsock2 headers this translation unit already has.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "common/Settings.h"
#include "common/SingleInstanceLock.h"
#include "coordination/KeyboardRescue.h"
#include "deskflow/App.h"
#include "deskflow/ClientApp.h"
#include "deskflow/ServerApp.h"
#include "deskflow/MouserLink.h"
#include "deskflow/ipc/CoreIpc.h"
#include "deskflow/ipc/CoreIpcServer.h"

#if defined(Q_OS_WIN)
#include "arch/win32/ArchMiscWindows.h"
#endif

#if defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif

#include <QApplication>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>

void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
  const auto utf8 = message.toUtf8();
  switch (type) {
  case QtDebugMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_DEBUG "%s", utf8.constData());
    break;
  case QtInfoMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_INFO "%s", utf8.constData());
    break;
  case QtWarningMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_WARN "%s", utf8.constData());
    break;
  case QtCriticalMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_ERR "%s", utf8.constData());
    break;
  case QtFatalMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_CRIT "%s", utf8.constData());
    break;
  }
  if (type == QtFatalMsg) {
    abort();
  }
}

// Runs on the main (Qt) thread: IpcServer::hasClients() is only safe there;
// the coordinator reads are mutex/atomic-guarded.
void logHealthLine(
    const AutoModeRunner &runner, const deskflow::core::ipc::CoreIpcServer &ipcServer,
    std::chrono::steady_clock::time_point startedAt
)
{
  using deskflow::coordination::Role;
  deskflow::core::health::Snapshot snapshot;
  snapshot.seat = Settings::value(Settings::Core::ComputerName).toString().toStdString();
  snapshot.epoch = runner.epochCount();
  snapshot.upSeconds = static_cast<long>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt).count()
  );
  Role role = Role::Init;
  if (const auto *coordinator = runner.healthCoordinator()) {
    role = coordinator->runningRole();
    snapshot.stats = coordinator->healthStats();
  }
  snapshot.role = deskflow::coordination::roleName(role);
  snapshot.tap = deskflow::core::health::tapState(role, snapshot.stats.relayRunning);
#if defined(Q_OS_MAC)
  snapshot.ax = AXIsProcessTrusted() ? "trusted" : "no";
#else
  snapshot.ax = "n/a";
#endif
  snapshot.guiIpc = ipcServer.hasClients();
  const auto &mouser = deskflow::MouserLink::shared();
  snapshot.mouserBridge = deskflow::core::health::mouserBridgeState(
      mouser.connected(), mouser.mode() == deskflow::MouserLink::Mode::Legacy
  );
  LOG_INFO("%s", deskflow::core::health::formatLine(snapshot).c_str());
}

void showHelp(const CoreArgParser &parser)
{
  QTextStream(stdout) << parser.helpText();
}

App *createApp(const CoreArgParser &parser, EventQueue &events, const QString &processName)
{
  if (parser.serverMode()) {
    return new ServerApp(&events, processName);
  } else if (parser.clientMode()) {
    return new ClientApp(&events, processName);
  }
  return nullptr;
}

int main(int argc, char **argv)
{
  // `--check-permissions` must run before Qt/app init so it works from a
  // headless SSH session (no display, no TCC prompt).
  if (const int rc = deskflow::core::permissions::run(argc, argv, deskflow::core::permissions::probe, std::cout);
      rc >= 0) {
    return rc;
  }

#if defined(Q_OS_WIN)
  ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));
#endif

  QApplication::setApplicationName(QStringLiteral("%1 Core").arg(kAppName));
  QApplication app(argc, argv);

#if defined(Q_OS_MAC)
  // The core is always a background process; without this transform the
  // auto-mode core registers as a Foreground app and shows a Dock icon
  // (App::run() only applies it on the GUI-spawned server/client path).
  ProcessSerialNumber psn = {0, kCurrentProcess};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  GetCurrentProcess(&psn);
  TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#pragma GCC diagnostic pop
#endif

  Arch arch;
  arch.init();

  Log log;
  qInstallMessageHandler(qtMessageHandler);

  CoreArgParser parser(QCoreApplication::arguments());

  // Print any parser errors
  if (!parser.errorText().isEmpty()) {
    QTextStream(stdout) << parser.errorText() << "\n";
  }

  if (parser.help()) {
    showHelp(parser);
    return s_exitSuccess;
  }

  if (parser.version()) {
    QTextStream(stdout) << parser.versionText();
    return s_exitSuccess;
  }

  // Before we check any more args we need to check for a duplicate process.
  // Two locks, both held for the life of the process (the kernel releases
  // them on death, so a crash never leaves a stale guard):
  //   Session -- one core per logged-in user.
  //   Machine -- one core per host across users/sessions, so a root or
  //              other-user core and this user's core cannot both grab the
  //              input hooks. Lock files are named per role, so this only
  //              serializes core-vs-core; it does NOT exclude the LoginWindow
  //              vhid-bridge (it holds "vhid-bridge.*", a different name).
  //              Bridge exclusion comes from launchd tearing down the
  //              LoginWindow session at login and the bridge's release_all()
  //              on exit. The 3 s wait only covers a previous core (e.g. the
  //              one kickstart -k just killed) still dropping its lock.
  std::optional<deskflow::SingleInstanceLock> sessionLock;
  std::optional<deskflow::SingleInstanceLock> machineLock;
  if (parser.singleInstanceOnly()) {
    using deskflow::SingleInstanceLock;
    sessionLock = SingleInstanceLock::tryAcquire(SingleInstanceLock::Role::Core, SingleInstanceLock::Scope::Session);
    if (!sessionLock) {
      LOG_ERR("an instance of deskflow core is already running: %s", SingleInstanceLock::lastMessage().c_str());
      return s_exitDuplicate;
    }
    machineLock = SingleInstanceLock::tryAcquire(
        SingleInstanceLock::Role::Core, SingleInstanceLock::Scope::Machine, std::chrono::seconds(3)
    );
    if (!machineLock) {
      LOG_ERR(
          "an instance of deskflow core is already running in another session: %s",
          SingleInstanceLock::lastMessage().c_str()
      );
      return s_exitDuplicate;
    }
    if (const auto msg = SingleInstanceLock::lastMessage(); !msg.empty()) {
      LOG_WARN("%s", msg.c_str());
    }
  }

#if defined(Q_OS_WIN)
  // Input injection is latency-critical: every millisecond this process
  // waits to be scheduled is visible cursor lag, and the desk thread
  // already asks for THREAD_PRIORITY_ABOVE_NORMAL -- which a NORMAL
  // process class caps. Raising the class lets that elevation actually
  // take effect, so a busy background app (screen capture, indexing)
  // cannot delay injected input. ABOVE_NORMAL needs no special rights;
  // HIGH/REALTIME would starve the rest of the desktop.
  if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
    LOG_WARN("could not raise process priority: %lu", GetLastError());
  }
#endif

  parser.parse();

  EventQueue events;
  const auto processName = QFileInfo(argv[0]).fileName();

  if (parser.autoMode()) {
    // Coordinated mode: the epoch loop elects and runs the role in-process.
    AutoModeRunner runner(events, processName);

    const auto ipcServer = new deskflow::core::ipc::CoreIpcServer(&app); // NOSONAR - Qt managed
    deskflow::coordination::setLocalCoreRestartHandler(&ipcRequestLocalCoreRestart);
    QObject::connect(ipcServer, &deskflow::core::ipc::IpcServer::stopProcessRequested, &app, [&runner] {
      runner.requestQuit();
    });
#if SYSAPI_WIN32
    AppUtilWindows::setProcessQuitHandler([&runner] { runner.requestQuit(); });
#endif
    ipcServer->listen();

    const auto startedAt = std::chrono::steady_clock::now();
    auto *healthTimer = new QTimer(&app); // NOSONAR - Qt managed
    QObject::connect(healthTimer, &QTimer::timeout, &app, [&runner, ipcServer, startedAt] {
      logHealthLine(runner, *ipcServer, startedAt);
    });
    healthTimer->start(std::chrono::duration_cast<std::chrono::milliseconds>(deskflow::core::health::kInterval));

    QThread coreThread;
    QObject::connect(&coreThread, &QThread::finished, &app, &QApplication::quit);
    runner.run(coreThread);

    int exitCode = QApplication::exec();
    coreThread.wait();

    if (exitCode == s_exitSuccess) {
      exitCode = runner.exitCode();
    }
    LOG_DEBUG("core exited, code: %d", exitCode);
    return exitCode;
  }

  std::unique_ptr<App> coreApp(createApp(parser, events, processName));
  if (coreApp == nullptr) {
    LOG_ERR("deskflow core requires --server or --client mode");
    return s_exitArgs;
  }

  const auto ipcServer = new deskflow::core::ipc::CoreIpcServer(&app); // NOSONAR - Qt managed
  deskflow::coordination::setLocalCoreRestartHandler(&ipcRequestLocalCoreRestart);
  QObject::connect(
      ipcServer, &deskflow::core::ipc::IpcServer::stopProcessRequested, coreApp.get(), &App::quit, Qt::DirectConnection
  );
  ipcServer->listen();

  QThread coreThread;
  QObject::connect(&coreThread, &QThread::finished, &app, &QApplication::quit);
  coreApp->run(coreThread);

  int exitCode = QApplication::exec();
  coreThread.wait();

  if (exitCode == s_exitSuccess) {
    exitCode = coreApp->getExitCode();
  }

  LOG_DEBUG("core exited, code: %d", exitCode);
  return exitCode;
}
