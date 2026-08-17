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
#include <QTemporaryFile>

namespace deskflow::gui {

namespace {

const auto kAgentLabel = QStringLiteral("org.deskflow.vhid-bridge");
const auto kBridgeLogPath = QStringLiteral("/var/log/deskflow-vhid-bridge.log");
// Retired pre-native-mode login-window coordinator. Enabling the bridge
// removes it so the two never both run at the login screen.
const auto kLegacyAgentPlist = QStringLiteral("/Library/LaunchAgents/com.kvm.autoswitch.loginwindow.plist");
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
bool runPrivileged(const QString &shellCommand, QString *error);

/// Install the staged LaunchAgent plist (admin prompt).
bool installAgentPlist(const QString &stagedPath, QString *error)
{
  const auto agentPlist = QStringLiteral("/Library/LaunchAgents/%1.plist").arg(kAgentLabel);
  const auto command =
      QStringLiteral("install -d /Library/LaunchAgents && install -m 644 -o root -g wheel '%1' '%2' && "
                     "rm -f '%3'; pkill -f '.kvm-autoswitch/coordinator.py' || true")
          .arg(stagedPath, agentPlist, kLegacyAgentPlist);
  return runPrivileged(command, error);
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

QStringList LoginBridgeManager::serverCandidates()
{
  const auto selfName = Settings::value(Settings::Core::ComputerName).toString().trimmed();
  const auto peersValue = Settings::value(Settings::Coordination::Peers).toStringList().join(',');

  QStringList hosts;
  // Peer entries: `name` or `name=address[|lanAddress]`. Any peer can be the
  // elected server; collect every address form (the bridge cycles them).
  for (const auto &rawEntry : peersValue.split(',', Qt::SkipEmptyParts)) {
    const auto entry = rawEntry.trimmed();
    if (entry.isEmpty())
      continue;
    const auto eq = entry.indexOf('=');
    const auto name = (eq < 0 ? entry : entry.left(eq)).trimmed();
    if (name.compare(selfName, Qt::CaseInsensitive) == 0)
      continue;
    if (eq < 0) {
      hosts.append(name);
      continue;
    }
    for (const auto &addr : entry.mid(eq + 1).split('|', Qt::SkipEmptyParts)) {
      const auto trimmed = addr.trimmed();
      if (!trimmed.isEmpty() && !hosts.contains(trimmed))
        hosts.append(trimmed);
    }
  }
  return hosts;
}

QString LoginBridgeManager::plistContent(double scale)
{
  const auto hosts = serverCandidates();
  const auto screenName = Settings::value(Settings::Core::ComputerName).toString();
  const auto port = Settings::value(Settings::Core::Port).toInt();

  // LimitLoadToSessionType=LoginWindow scopes the agent to login-window
  // sessions only: launchd starts it at the login screen and tears it down
  // when a user session takes over (where deskflow-core handles input).
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>%1</string>
  <key>ProgramArguments</key>
  <array>
    <string>%2</string>
    <string>%3</string>
    <string>%4</string>
    <string>%5</string>
    <string>--scale=%6</string>
  </array>
  <key>LimitLoadToSessionType</key><string>LoginWindow</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/var/log/deskflow-vhid-bridge.log</string>
  <key>StandardErrorPath</key><string>/var/log/deskflow-vhid-bridge.log</string>
</dict>
</plist>
)")
      .arg(kAgentLabel, bridgePath(), hosts.join(','), screenName, QString::number(port), QString::number(scale));
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
  if (serverCandidates().isEmpty()) {
    if (reason)
      *reason = QObject::tr("no coordination peers configured — add the other computers first");
    return false;
  }
  return true;
}

bool LoginBridgeManager::runInstallScript(double scale, QString *error)
{
  if (!canInstall(error))
    return false;

  Settings::setValue(Settings::Coordination::LoginBridgeScale, scale);
  Settings::setValue(Settings::Coordination::LoginBridgeEnabled, true);
  Settings::save(false);

  QTemporaryFile staged;
  if (!staged.open() || staged.write(plistContent(scale).toUtf8()) < 0) {
    if (error)
      *error = QObject::tr("could not stage the agent plist");
    return false;
  }
  staged.flush();

  if (!installAgentPlist(staged.fileName(), error))
    return false;
  return agentInstalled();
}

bool LoginBridgeManager::apply(bool enabled, double scale, QString *error)
{
  if (enabled == agentInstalled() && !enabled)
    return true;

  if (!enabled) {
    const auto command = QStringLiteral("rm -f '%1'; pkill -f deskflow-vhid-bridge || true").arg(agentPlistPath());
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
  return QString::fromUtf8(onDisk.readAll()).simplified() == plistContent(scale).simplified();
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
