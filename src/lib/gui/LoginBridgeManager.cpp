/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LoginBridgeManager.h"

#include "common/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace deskflow::gui {

namespace {

const auto kAgentLabel = QStringLiteral("org.deskflow.vhid-bridge");
const auto kBridgeLogPath = QStringLiteral("/var/log/deskflow-vhid-bridge.log");
const auto kDaemonAppPath = QStringLiteral(
    "/Library/Application Support/org.pqrs/Karabiner-DriverKit-VirtualHIDDevice/"
    "Applications/Karabiner-VirtualHIDDevice-Daemon.app"
);

/// Escape a shell command for embedding in an AppleScript string literal.
QString appleScriptQuote(const QString &shellCommand)
{
  QString escaped = shellCommand;
  escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
  escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
  return escaped;
}

/// Keep the Qt event loop alive while osascript shows the admin password sheet.
bool waitForProcessWithEvents(QProcess &proc, int timeoutMs, QString *error)
{
  QElapsedTimer timer;
  timer.start();
  while (!proc.waitForFinished(100)) {
    if (QCoreApplication::instance() != nullptr) {
      QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    if (timer.elapsed() > timeoutMs) {
      proc.kill();
      if (error)
        *error = QStringLiteral("timed out waiting for administrator approval");
      return false;
    }
  }
  return true;
}

/// Run a shell command with an admin prompt (osascript). Returns true on
/// success; fills @p error with stderr / cancellation reason otherwise.
bool runPrivileged(const QString &shellCommand, QString *error)
{
  const QString script = QStringLiteral("do shell script \"%1\" with administrator privileges")
                             .arg(appleScriptQuote(shellCommand));
  QProcess osascript;
  osascript.start(QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), script});
  if (!waitForProcessWithEvents(osascript, 120000, error)) {
    return false;
  }
  if (osascript.exitCode() != 0) {
    if (error) {
      const auto stderrText = QString::fromUtf8(osascript.readAllStandardError()).trimmed();
      *error = stderrText.contains(QStringLiteral("User cancelled"), Qt::CaseInsensitive)
          ? QStringLiteral("the administrator prompt was cancelled")
          : stderrText;
    }
    return false;
  }
  return true;
}

/// Run the bundled install script. With @p dryRun the script renders the plist
/// to stdout and installs nothing; otherwise it installs behind its own admin
/// prompt. Settings are flushed first: the script reads Deskflow.conf, not memory.
bool runBridgeScript(double scale, bool dryRun, QString *output, QString *error)
{
  const auto script = LoginBridgeManager::installScriptPath();
  if (script.isEmpty()) {
    if (error)
      *error = QObject::tr("install-login-bridge-macos.sh is not bundled with this build");
    return false;
  }
  Settings::save(false);

  QStringList args = {script, QStringLiteral("--scale"), QString::number(scale)};
  if (dryRun)
    args.append(QStringLiteral("--dry-run"));

  QProcess proc;
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("DESKFLOW_SETTINGS"), Settings::settingsFile());
  proc.setProcessEnvironment(env);
  proc.start(QStringLiteral("/bin/bash"), args);
  if (!waitForProcessWithEvents(proc, dryRun ? 15000 : 120000, error)) {
    return false;
  }
  const auto stderrText = QString::fromUtf8(proc.readAllStandardError()).trimmed();
  if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
    if (error) {
      *error = stderrText.contains(QStringLiteral("User cancelled"), Qt::CaseInsensitive)
          ? QStringLiteral("the administrator prompt was cancelled")
          : (stderrText.isEmpty() ? QStringLiteral("install script exited %1").arg(proc.exitCode()) : stderrText);
    }
    return false;
  }
  if (output)
    *output = QString::fromUtf8(proc.readAllStandardOutput());
  return true;
}

} // namespace

bool LoginBridgeManager::driverInstalled()
{
  return QFile::exists(kDaemonAppPath);
}

bool LoginBridgeManager::daemonRunning()
{
  QProcess pgrep;
  pgrep.start(QStringLiteral("/usr/bin/pgrep"), {QStringLiteral("-f"), QStringLiteral("Karabiner-VirtualHIDDevice-Daemon")});
  pgrep.waitForFinished(3000);
  return pgrep.exitCode() == 0;
}

bool LoginBridgeManager::agentInstalled()
{
  return QFile::exists(agentPlistPath());
}

QString LoginBridgeManager::driverDownloadUrl()
{
  return QStringLiteral("https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice/releases");
}

QString LoginBridgeManager::statusText()
{
  if (!QFile::exists(bridgePath()))
    return QObject::tr("Bridge binary missing from this build");
  if (!driverInstalled())
    return QObject::tr("Karabiner driver not installed");
  if (!daemonRunning())
    return QObject::tr("Karabiner driver installed, daemon not running");
  if (agentInstalled())
    return QObject::tr("Active (driver running, login-window agent installed)");
  return QObject::tr("Ready to enable (driver running)");
}

QString LoginBridgeManager::bridgePath()
{
  return QCoreApplication::applicationDirPath() + QStringLiteral("/deskflow-vhid-bridge");
}

QString LoginBridgeManager::agentPlistPath()
{
  return QStringLiteral("/Library/LaunchAgents/%1.plist").arg(kAgentLabel);
}

QString LoginBridgeManager::installScriptPath()
{
  const QFileInfo bundled(
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../Resources/install-login-bridge-macos.sh"))
  );
  if (bundled.exists())
    return bundled.canonicalFilePath();

  // Dev builds without a bundled copy: repo script relative to build output.
  const QFileInfo devTree(QDir(QCoreApplication::applicationDirPath()).filePath(
      QStringLiteral("../../../../../scripts/install-login-bridge-macos.sh")
  ));
  if (devTree.exists())
    return devTree.canonicalFilePath();

  return {};
}

bool LoginBridgeManager::canInstall(QString *reason)
{
  if (!driverInstalled()) {
    if (reason)
      *reason = QObject::tr("Karabiner driver not installed");
    return false;
  }
  if (!QFile::exists(bridgePath())) {
    if (reason)
      *reason = QObject::tr("bridge binary not found at %1").arg(bridgePath());
    return false;
  }
  // The script owns peer parsing; a render that fails (no peers, missing name) is the reason.
  return renderAgentPlist(Settings::value(Settings::Coordination::LoginBridgeScale).toDouble(), nullptr, reason);
}

bool LoginBridgeManager::renderAgentPlist(double scale, QString *plist, QString *error)
{
  return runBridgeScript(scale, true, plist, error);
}

bool LoginBridgeManager::runInstallScript(double scale, QString *error)
{
  if (!canInstall(error))
    return false;

  Settings::setValue(Settings::Coordination::LoginBridgeScale, scale);
  Settings::setValue(Settings::Coordination::LoginBridgeEnabled, true);

  if (!runBridgeScript(scale, false, nullptr, error))
    return false;
  return agentInstalled();
}

bool LoginBridgeManager::apply(bool enabled, double scale, QString *error)
{
  if (enabled == agentInstalled() && !enabled)
    return true;

  if (!enabled) {
    // bootout by label: SIGTERM lets the bridge release its keys and say goodbye
    // to the server; pkill -f matched any command line containing the name.
    const auto command = QStringLiteral("rm -f '%1'; launchctl bootout loginwindow/%2 2>/dev/null || true")
                             .arg(agentPlistPath(), kAgentLabel);
    return runPrivileged(command, error);
  }

  return runInstallScript(scale, error);
}

bool LoginBridgeManager::installedAgentMatchesCurrentSettings(double scale)
{
  if (!agentInstalled()) {
    return false;
  }
  QFile onDisk(agentPlistPath());
  if (!onDisk.open(QIODevice::ReadOnly)) {
    return false;
  }
  QString rendered;
  if (!renderAgentPlist(scale, &rendered, nullptr)) {
    return false;
  }
  return QString::fromUtf8(onDisk.readAll()).simplified() == rendered.simplified();
}

QString LoginBridgeManager::recentLogText(int maxLines)
{
  if (maxLines < 1) {
    maxLines = 1;
  }
  QFile log(kBridgeLogPath);
  if (!log.open(QIODevice::ReadOnly)) {
    return QObject::tr("(no bridge log yet — enable the agent, then logout or restart to test)");
  }
  const auto lines = QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
  if (lines.isEmpty()) {
    return QObject::tr("(bridge log is empty)");
  }
  return lines.mid(qMax(0, lines.size() - maxLines)).join('\n');
}

} // namespace deskflow::gui
