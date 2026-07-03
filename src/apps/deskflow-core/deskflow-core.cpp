/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2025 - 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AutoModeRunner.h"
#include "CoreArgParser.h"

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "deskflow/App.h"
#include "deskflow/ClientApp.h"
#include "deskflow/ServerApp.h"
#include "deskflow/ipc/CoreIpcServer.h"

#if defined(Q_OS_WIN)
#include "arch/win32/ArchMiscWindows.h"
#endif

#if defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif

#include <QApplication>
#include <QFileInfo>
#include <QSharedMemory>
#include <QTextStream>
#include <QThread>

#include <memory>

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
  // Create a shared memory segment with a unique key
  // This is to prevent a new instance from running if one is already running
  QSharedMemory sharedMemory(kCoreBinName);

  // Attempt to attach first and detach in order to clean up stale shm chunks
  // This can happen if the previous instance was killed or crashed
  if (sharedMemory.attach())
    sharedMemory.detach();

  if (!sharedMemory.create(1) && parser.singleInstanceOnly()) {
    LOG_WARN("an instance of deskflow core is already running");
    return s_exitDuplicate;
  }

#if defined(Q_OS_WIN)
  // QSharedMemory lives in the per-session Local\ kernel namespace, so a
  // login-screen (SYSTEM/elevated) core and a user-session core can coexist
  // and fight over keyboard hooks and the mesh identity. A Global\ mutex
  // dedupes across sessions; the watchdog owns replacing a stale core.
  HANDLE globalMutex = CreateMutexW(nullptr, TRUE, L"Global\\deskflow-core-single-instance");
  const DWORD mutexError = GetLastError();
  if (parser.singleInstanceOnly() &&
      ((globalMutex != nullptr && mutexError == ERROR_ALREADY_EXISTS) ||
       (globalMutex == nullptr && mutexError == ERROR_ACCESS_DENIED))) {
    // ACCESS_DENIED: the mutex exists but was created at a higher integrity
    // level (elevated/SYSTEM core) -- still a duplicate.
    LOG_WARN("an instance of deskflow core is already running in another session");
    if (globalMutex != nullptr) {
      CloseHandle(globalMutex);
    }
    return s_exitDuplicate;
  }
#endif

  parser.parse();

  EventQueue events;
  const auto processName = QFileInfo(argv[0]).fileName();

  if (parser.autoMode()) {
    // Coordinated mode: the epoch loop elects and runs the role in-process.
    AutoModeRunner runner(events, processName);

    const auto ipcServer = new deskflow::core::ipc::CoreIpcServer(&app); // NOSONAR - Qt managed
    QObject::connect(ipcServer, &deskflow::core::ipc::IpcServer::stopProcessRequested, &app, [&runner] {
      runner.requestQuit();
    });
    ipcServer->listen();

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
