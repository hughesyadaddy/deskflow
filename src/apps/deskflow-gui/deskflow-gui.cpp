/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "common/I18N.h"
#include "common/PlatformInfo.h"
#include "common/SingleInstanceLock.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/Diagnostic.h"
#include "gui/InstanceHandoff.h"
#include "gui/MainWindow.h"
#include "gui/Messages.h"
#include "gui/StyleUtils.h"

#if defined(Q_OS_MACOS)
#include "gui/LaunchOwnership.h"
#include "gui/OSXHelpers.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>

#if defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#include <chrono>
#include <cstdlib>
#endif

#if defined(Q_OS_UNIX) && defined(QT_DEBUG)
#include <QLoggingCategory>
#endif

#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
#include "platform/XDGPortalRegistry.h"
#endif

using namespace deskflow::gui;

#if defined(Q_OS_MACOS)
bool checkMacAssistiveDevices();
#endif

const static auto kHeader = QStringLiteral("%1: %2\n").arg(kAppName, kDisplayVersion);

int main(int argc, char *argv[])
{
#if defined(Q_OS_UNIX) && defined(QT_DEBUG)
  // Fixes Fedora bug where qDebug() messages aren't printed.
  QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true\nqt.*=false"));
#endif

#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
  deskflow::platform::setAppId();
#endif

  QCoreApplication::setApplicationName(kAppName);
  QCoreApplication::setOrganizationName(kAppName);
  QCoreApplication::setApplicationVersion(kVersion);
  QCoreApplication::setOrganizationDomain(kOrgDomain); // used in prefix, can't be a url
  QGuiApplication::setDesktopFileName(kRevFqdnName);

  QApplication app(argc, argv);
  // Tray-resident app: hiding the last window (or closing a stray dialog
  // while the main window is hidden) must never terminate the process.
  // Quit happens explicitly via the tray menu (QApplication::quit()).
  QApplication::setQuitOnLastWindowClosed(false);

  // Ensure the I18N object is made before strings
  QTextStream(stdout) << "initial language: " << I18N::currentLanguage() << '\n';

  // Add Command Line Options
  auto helpOption = QCommandLineOption({"h", "help"}, "Display Help on the command line");
  auto versionOption = QCommandLineOption({"v", "version"}, "Display version information");
  auto resetOption = QCommandLineOption("reset", "Reset all settings");
  auto showOption = QCommandLineOption("show", "Show the main window on launch (ignores autohide)");

  QCommandLineParser parser;
  parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
  parser.addOption(helpOption);
  parser.addOption(versionOption);
  parser.addOption(resetOption);
  parser.addOption(showOption);
  parser.parse(QCoreApplication::arguments());

  if (!parser.errorText().isEmpty()) {
    qCritical().noquote() << parser.errorText() << "\nUse --help for more information.";
    return s_exitArgs;
  }

  if (parser.isSet(helpOption)) {
    QTextStream(stdout) << kHeader << QStringLiteral("  %1\n\n").arg(kAppDescription)
                        << parser.helpText().replace(QApplication::applicationFilePath(), kAppId);
    return s_exitSuccess;
  }

  if (parser.isSet(versionOption)) {
    QTextStream(stdout) << kHeader << kCopyright << Qt::endl;
    return s_exitSuccess;
  }

  // One GUI per logged-in user. The lock is a kernel-released flock/mutex
  // held for the life of the process, so a crashed GUI never blocks the next
  // launch. MainWindow only removes/relistens the "raise window" QLocalServer
  // after this point, i.e. only once we are provably the sole instance.
  using deskflow::SingleInstanceLock;
  using deskflow::gui::InstanceHandoffServer;
  const auto socketName = QStringLiteral("%1-gui").arg(kAppId);
  auto instanceLock = SingleInstanceLock::tryAcquire(SingleInstanceLock::Role::Gui, SingleInstanceLock::Scope::Session);
#if defined(Q_OS_MACOS)
  const auto ownership = deskflow::gui::decideLaunchOwnership(
      macLaunchdOwnsGui(), macGuiLaunchAgentInstalled(), macStartAtLoginEnabled()
  );
  if (!instanceLock && ownership.mayTakeOver) {
    // launchd's copy is canonical. Exit 5 here would make a KeepAlive agent relaunch
    // us every 30 s for as long as an unmanaged (Login Item) copy lives; instead ask
    // that copy to quit and take its place. Exit 0 keeps launchd quiet if it will not.
    // An unmanaged copy never takes over: it exits 5 below like any duplicate.
    if (InstanceHandoffServer::requestQuit(socketName)) {
      instanceLock = SingleInstanceLock::tryAcquire(
          SingleInstanceLock::Role::Gui, SingleInstanceLock::Scope::Session, std::chrono::milliseconds(5000)
      );
    }
    if (!instanceLock) {
      qWarning("another gui instance holds the lock and would not hand over; exiting quietly");
      return s_exitSuccess;
    }
    qInfo("took over from an unmanaged gui instance");
  }
#endif
  if (!instanceLock) {
    // Ping the running instance to have it show itself
    if (!InstanceHandoffServer::requestShow(socketName)) {
      // If we can't connect to the other instance tell the user its running.
      // This should never happen but just incase we should show something
      QMessageBox::information(nullptr, kAppName, QObject::tr("%1 is already running").arg(kAppName));
    }
    return s_exitDuplicate;
  }

  if (!deskflow::platform::isMac() && qEnvironmentVariable("XDG_CURRENT_DESKTOP") != QLatin1String("KDE")) {
    QApplication::setStyle("fusion");
  }

  // Sets the fallback icon path and fallback theme
  updateIconTheme();

  qInstallMessageHandler(deskflow::gui::messages::messageHandler);
  qInfo("%s v%s", kAppName, kDisplayVersion);

#if defined(Q_OS_MACOS)

  if (app.applicationDirPath().startsWith("/Volumes/")) {
    QString msgBody = QStringLiteral(
        "Please drag %1 to the Applications folder, "
        "and open it from there."
    );
    QMessageBox::information(nullptr, kAppName, msgBody.arg(kAppName));
    return 1;
  }

  // Not trusted yet (every re-signed deploy resets TCC): the native prompt is shown
  // once and the tray still comes up so the app is not silently absent.
  const bool accessibilityGranted = checkMacAssistiveDevices();
#endif

  // --no-reset
  if (parser.isSet(resetOption)) {
    diagnostic::clearSettings(false);
  }

#if defined(Q_OS_MACOS)
  // Start as a menu-bar/tray app; showEvent promotes to Regular when the window opens.
  macOSSetDockVisible(false);
#endif

  MainWindow mainWindow;
#if defined(Q_OS_MACOS)
  if (!accessibilityGranted) {
    mainWindow.openWhenAccessibilityGranted(parser.isSet(showOption));
  } else {
    mainWindow.open(parser.isSet(showOption));
  }
#else
  mainWindow.open(parser.isSet(showOption));
#endif

  return QApplication::exec();
}

#if defined(Q_OS_MACOS)
bool checkMacAssistiveDevices()
{
  // new in mavericks, applications are trusted individually
  // with use of the accessibility api. this call will show a
  // prompt which can show the security/privacy/accessibility
  // tab, with a list of allowed applications. deskflow should
  // show up there automatically, but will be unchecked.

  if (AXIsProcessTrusted()) {
    return true;
  }

  const void *keys[] = {kAXTrustedCheckOptionPrompt};
  const void *trueValue[] = {kCFBooleanTrue};
  CFDictionaryRef options = CFDictionaryCreate(nullptr, keys, trueValue, 1, nullptr, nullptr);

  bool result = AXIsProcessTrustedWithOptions(options);
  CFRelease(options);
  return result;
}
#endif
