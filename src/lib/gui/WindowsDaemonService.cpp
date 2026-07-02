/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WindowsDaemonService.h"

#include "arch/ArchException.h"
#include "arch/win32/ArchDaemonWindows.h"
#include "arch/win32/ArchMiscWindows.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "common/Settings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QObject>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>

namespace deskflow::gui {

namespace {

QString daemonExecutablePath()
{
  return QStringLiteral("%1/%2.exe").arg(QCoreApplication::applicationDirPath(), kDaemonBinName);
}

bool runElevatedServiceInstall(const QString &guiPath)
{
  const auto parameters = QStringLiteral("--install-daemon-service");
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = guiPath.toStdWString().c_str();
  info.lpParameters = parameters.toStdWString().c_str();
  info.nShow = SW_HIDE;

  if (!ShellExecuteExW(&info)) {
    const auto error = GetLastError();
    if (error == ERROR_CANCELLED) {
      qWarning("Deskflow service install cancelled at the UAC prompt");
    } else {
      qWarning("failed to launch elevated Deskflow service install: %lu", error);
    }
    return false;
  }

  if (info.hProcess == nullptr) {
    return false;
  }

  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(info.hProcess, &exitCode);
  CloseHandle(info.hProcess);
  return exitCode == 0;
}

bool ensureInstalled()
{
  if (ArchDaemonWindows::isServiceInstalled(kAppName)) {
    return true;
  }

  const auto daemonPath = daemonExecutablePath();
  if (!QFile::exists(daemonPath)) {
    qWarning("deskflow-daemon.exe not found at %s", qPrintable(daemonPath));
    return false;
  }

  if (ArchMiscWindows::isProcessElevated()) {
    try {
      ArchDaemonWindows::installService(kAppName, daemonPath);
    } catch (ArchDaemonFailedException &e) {
      qWarning("failed to install Deskflow background service: %s", e.what());
      return false;
    }
  } else if (!runElevatedServiceInstall(QCoreApplication::applicationFilePath())) {
    return false;
  }

  if (!ArchDaemonWindows::isServiceInstalled(kAppName)) {
    qWarning("Deskflow background service is still not installed after setup");
    return false;
  }

  qInfo("Deskflow background service installed");
  return true;
}

} // namespace

bool WindowsDaemonService::installAndStart()
{
  if (!ensureInstalled()) {
    return false;
  }

  try {
    ArchDaemonWindows::setServiceStartType(kAppName, true);
    if (!ArchDaemonWindows::isServiceRunning(kAppName)) {
      qInfo("starting Deskflow background service");
      ArchDaemonWindows::startService(kAppName);
    }
    return ArchDaemonWindows::isServiceRunning(kAppName);
  } catch (ArchDaemonFailedException &e) {
    qWarning("failed to start Deskflow background service: %s", e.what());
    return false;
  }
}

int WindowsDaemonService::installFromCli()
{
  if (!installAndStart()) {
    return s_exitFailed;
  }
  return s_exitSuccess;
}

QString WindowsDaemonService::statusText()
{
  if (!ArchDaemonWindows::isServiceInstalled(kAppName)) {
    return QCoreApplication::translate("WindowsDaemonService", "Background service: not installed");
  }
  if (ArchDaemonWindows::isServiceRunning(kAppName)) {
    return QCoreApplication::translate("WindowsDaemonService", "Background service: running");
  }
  return QCoreApplication::translate("WindowsDaemonService", "Background service: installed but stopped");
}

bool WindowsDaemonService::isRunning()
{
  return ArchDaemonWindows::isServiceInstalled(kAppName) && ArchDaemonWindows::isServiceRunning(kAppName);
}

bool WindowsDaemonService::ensureRunning()
{
  if (Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() != Settings::ProcessMode::Service) {
    return true;
  }

  syncServiceStartTypeFromSettings();

  if (ArchDaemonWindows::isServiceRunning(kAppName)) {
    qDebug("Deskflow background service already running");
    return true;
  }

  return installAndStart();
}

void WindowsDaemonService::syncServiceStartTypeFromSettings()
{
  if (Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() != Settings::ProcessMode::Service) {
    return;
  }

  if (!ArchDaemonWindows::isServiceInstalled(kAppName)) {
    return;
  }

  try {
    ArchDaemonWindows::setServiceStartType(
        kAppName,
        Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() == Settings::ProcessMode::Service
    );
    qDebug("Deskflow background service start type synced to process mode");
  } catch (ArchDaemonFailedException &e) {
    qWarning("failed to configure Deskflow service start type: %s", e.what());
  }
}

void WindowsDaemonService::stop()
{
  if (Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() != Settings::ProcessMode::Service) {
    return;
  }

  // Match origin/master: the daemon stays running after the GUI exits so login-screen
  // and UAC paths (auto-elevate or VHID bridge) keep working when the tray app is closed.
  qDebug("keeping Deskflow background service running (service mode)");
}

} // namespace deskflow::gui
